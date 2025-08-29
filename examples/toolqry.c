/*
 * Copyright (c) 2025      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <libgen.h>

#include "examples.h"
#include <pmix_tool.h>

static void querycbfunc(pmix_status_t status, pmix_info_t *info, size_t ninfo, void *cbdata,
                        pmix_release_cbfunc_t release_fn, void *release_cbdata)
{
    myquery_data_t *mq = (myquery_data_t *) cbdata;
    size_t n;

    mq->lock.status = status;

    /* save the returned info - it will be
     * released in the release_fn */
    if (0 < ninfo) {
        PMIX_INFO_CREATE(mq->info, ninfo);
        mq->ninfo = ninfo;
        for (n = 0; n < ninfo; n++) {
            PMIX_INFO_XFER(&mq->info[n], &info[n]);
        }
    }

    /* let the library release the data */
    if (NULL != release_fn) {
        release_fn(release_cbdata);
    }

    /* release the block */
    DEBUG_WAKEUP_THREAD(&mq->lock);
}

int main(int argc, char **argv)
{
	pmix_status_t rc;
	char *kptr;
	pmix_rank_t rank = 0;
	pmix_info_t info[3], *iptr, *ip;
	pmix_proc_t myproc, proc;
	char hostname[1024];
	pmix_value_t *val = NULL;
	char *toolname;
    pmix_query_t *query;
    myquery_data_t mydata;
    size_t nq, ninfo, sz, m;
    pmix_data_array_t *darray, *dptr;
    int i;
    bool system_server = false;

    // check for directives
    for (i=1; i < argc; i++) {
        if (0 == strcmp(argv[i], "--system-server")) {
            system_server = true;
        }
    }

    /* assign our own name */
    ninfo = 2;
    gethostname(hostname, sizeof(hostname));
    toolname = basename(argv[0]);
    rc = asprintf(&kptr, "%s.%s.%lu", toolname, hostname, (unsigned long)getpid());
    if (0 > rc) {
        fprintf(stderr, "Failed to create name\n");
        exit(1);
    }
    PMIX_INFO_LOAD(&info[0], PMIX_TOOL_NSPACE, kptr, PMIX_STRING);
    free(kptr);
    PMIX_INFO_LOAD(&info[1], PMIX_TOOL_RANK, &rank, PMIX_PROC_RANK);
    // if they want us to use a system server, then say so
    if (system_server) {
        PMIX_INFO_LOAD(&info[2], PMIX_CONNECT_TO_SYSTEM, NULL, PMIX_BOOL);
        ninfo = 3;
    }

    /* init as a tool */
    rc = PMIx_tool_init(&myproc, info, ninfo);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_tool_init failed: %s\n", PMIx_Error_string(rc));
        exit(rc);
    }
    PMIX_PROC_CONSTRUCT(&proc);

    /* query the list of active nspaces */
    nq = 1;
    PMIX_QUERY_CREATE(query, nq);
    PMIX_ARGV_APPEND(rc, query[0].keys, PMIX_QUERY_NAMESPACE_INFO);
    DEBUG_CONSTRUCT_MYQUERY(&mydata);
    if (PMIX_SUCCESS != (rc = PMIx_Query_info_nb(query, nq, querycbfunc, (void *) &mydata))) {
        fprintf(stderr, "Client ns %s rank %d: PMIx_Query_info failed: %d\n", myproc.nspace,
                myproc.rank, rc);
        goto done;
    }
    DEBUG_WAIT_THREAD(&mydata.lock);
    rc = mydata.lock.status;
    /* find the response */
    if (PMIX_SUCCESS == rc) {
        /* should be in the first key */
        if (PMIX_CHECK_KEY(&mydata.info[0], PMIX_QUERY_NAMESPACE_INFO)) {
            darray = mydata.info[0].value.data.darray;
            if (NULL == darray || 0 == darray->size || NULL == darray->array) {
                fprintf(stderr, "No namespaces active\n");
                rc = PMIX_ERROR;
                goto done;
            } else {
                ip = (pmix_info_t *) darray->array;
                sz = darray->size;
                if (NULL == ip || 0 == sz) {
                    fprintf(stderr, "Error\n");
                    rc = PMIX_ERROR;
                    goto done;
                } else {
                    // find our namespace
                    for (m=0; m < sz; m++) {
                        dptr = ip[m].value.data.darray;
                        if (NULL == dptr || NULL == dptr->array || 0 == dptr->size) {
                            fprintf(stderr, "Error\n");
                            rc = PMIX_ERROR;
                            goto done;
                        }
                        iptr = (pmix_info_t *) dptr->array;
                        // looking for nspaces with an "@N" at the end
                        kptr = strrchr(iptr[0].value.data.string, '@');
                        if (NULL == kptr) {
                            continue;
                        }
                        ++kptr;
                        // ignore the "0" namespace as that is the DVM
                        i = strtol(kptr, NULL, 10);
                        if (0 == i) {
                            continue;
                        }
                        // take this one
                        PMIX_LOAD_PROCID(&proc, iptr[0].value.data.string, 0);
                        fprintf(stderr, "Received nspace %s\n", proc.nspace);
                        break;
                    }
                }
            }
        } else {
            fprintf(stderr, "Query returned wrong info key at first posn: %s\n",
                    mydata.info[0].key);
            rc = PMIX_ERROR;
            goto done;
        }
    } else {
        fprintf(stderr, "Query returned error: %s\n", PMIx_Error_string(mydata.lock.status));
        goto done;
    }
    DEBUG_DESTRUCT_MYQUERY(&mydata);

    rc = PMIx_Get(&proc, PMIX_APPNUM, NULL, 0, &val);
    fprintf(stderr, "APPNUM RETURN: %s\n", PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
    	fprintf(stderr, "\t%s\n", PMIx_Value_string(val));
    }

    rc = PMIx_Get(&proc, PMIX_LOCALLDR, NULL, 0, &val);
    fprintf(stderr, "LOCALLDR RETURN: %s\n", PMIx_Error_string(rc));
    if (PMIX_SUCCESS == rc) {
        fprintf(stderr, "\t%s\n", PMIx_Value_string(val));
    }

done:
    PMIx_Finalize(NULL, 0);
    return rc;
}
