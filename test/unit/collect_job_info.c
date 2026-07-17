/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Regression test for PMIx_server_collect_job_info().
 *
 * The internal handler behind this API accumulated each namespace's packed job
 * info into a local, per-iteration pmix_cb_t rather than into the caller's
 * caddy, so the API always returned an *empty* buffer to its consumers. This
 * test registers a namespace carrying job-level data, calls the API, and
 * asserts the returned data buffer is non-empty (and unpacks the leading
 * namespace name to confirm real content). Before the fix the buffer is empty
 * and this test fails; after the fix it carries the job info.
 */

#include "src/include/pmix_config.h"

#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        ++npass;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        ++nfail;
    }
}

/* Register a namespace with a minimal-but-valid job description: a
 * PMIX_JOB_INFO_ARRAY carrying the node/proc maps and job sizes, one
 * PMIX_NODE_INFO_ARRAY, and one PMIX_PROC_DATA entry per rank. This mirrors the
 * shape a real host builds in PMIx_server_register_nspace. */
static pmix_status_t register_job(const char *nspace, int nprocs)
{
    char *regex = NULL, *ppn = NULL, *rks, **agg = NULL;
    char tmp[64];
    pmix_info_t *info, *iptr, *pdata;
    pmix_data_array_t *array;
    size_t ninfo, n;
    int m, k, nnodes = 1;
    pmix_nspace_t ns;
    pmix_status_t rc;

    PMIx_generate_regex(pmix_globals.hostname, &regex);
    for (m = 0; m < nprocs; m++) {
        snprintf(tmp, sizeof(tmp), "%d", m);
        PMIx_Argv_append_nosize(&agg, tmp);
    }
    rks = PMIx_Argv_join(agg, ',');
    PMIx_Argv_free(agg);
    PMIx_generate_ppn(rks, &ppn);
    free(rks);

    ninfo = 1 /* job-info array */ + (size_t) nnodes + (size_t) nprocs;
    PMIX_INFO_CREATE(info, ninfo);

    n = 0;
    /* job-level info array */
    pmix_strncpy(info[n].key, PMIX_JOB_INFO_ARRAY, PMIX_MAX_KEYLEN);
    info[n].value.type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(info[n].value.data.darray, 8, PMIX_INFO);
    iptr = (pmix_info_t *) info[n].value.data.darray->array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_NODE_MAP, regex, PMIX_REGEX);
    PMIX_INFO_LOAD(&iptr[1], PMIX_PROC_MAP, ppn, PMIX_REGEX);
    PMIX_INFO_LOAD(&iptr[2], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[3], PMIX_JOBID, "1234", PMIX_STRING);
    PMIX_INFO_LOAD(&iptr[4], PMIX_UNIV_SIZE, &nprocs, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[5], PMIX_MAX_PROCS, &nprocs, PMIX_UINT32);
    m = 1;
    PMIX_INFO_LOAD(&iptr[6], PMIX_JOB_NUM_APPS, &m, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[7], PMIX_NUM_NODES, &nnodes, PMIX_UINT32);
    free(regex);
    free(ppn);
    ++n;

    /* one node-info array */
    pmix_strncpy(info[n].key, PMIX_NODE_INFO_ARRAY, PMIX_MAX_KEYLEN);
    info[n].value.type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(info[n].value.data.darray, 3, PMIX_INFO);
    iptr = (pmix_info_t *) info[n].value.data.darray->array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_HOSTNAME, pmix_globals.hostname, PMIX_STRING);
    m = 0;
    PMIX_INFO_LOAD(&iptr[1], PMIX_NODEID, &m, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[2], PMIX_NODE_SIZE, &nprocs, PMIX_UINT32);
    ++n;

    /* per-proc data */
    for (m = 0; m < nprocs; m++) {
        pmix_strncpy(info[n].key, PMIX_PROC_DATA, PMIX_MAX_KEYLEN);
        info[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(array, 3, PMIX_INFO);
        info[n].value.data.darray = array;
        pdata = (pmix_info_t *) array->array;
        k = 0;
        pmix_strncpy(pdata[k].key, PMIX_RANK, PMIX_MAX_KEYLEN);
        pdata[k].value.type = PMIX_PROC_RANK;
        pdata[k].value.data.rank = m;
        ++k;
        pmix_strncpy(pdata[k].key, PMIX_LOCAL_RANK, PMIX_MAX_KEYLEN);
        pdata[k].value.type = PMIX_UINT16;
        pdata[k].value.data.uint16 = m;
        ++k;
        pmix_strncpy(pdata[k].key, PMIX_NODEID, PMIX_MAX_KEYLEN);
        pdata[k].value.type = PMIX_UINT32;
        pdata[k].value.data.uint32 = 0;
        ++n;
    }

    PMIX_LOAD_NSPACE(ns, nspace);
    /* NULL cbfunc -> blocking registration */
    rc = PMIx_server_register_nspace(ns, nprocs, info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    return rc;
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    const char *nspace = "collect-job-test";
    pmix_proc_t procs[1];
    pmix_data_buffer_t dbuf;
    char *unpacked = NULL;
    int32_t cnt;
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== PMIx_server_collect_job_info unit test ===\n\n");

    rc = register_job(nspace, 2);
    /* a blocking (NULL cbfunc) registration reports success as either
     * PMIX_SUCCESS or PMIX_OPERATION_SUCCEEDED */
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stderr, "register_nspace failed: %s\n", PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 1;
    }

    PMIX_LOAD_PROCID(&procs[0], nspace, PMIX_RANK_WILDCARD);
    PMIX_DATA_BUFFER_CONSTRUCT(&dbuf);

    rc = PMIx_server_collect_job_info(procs, 1, &dbuf);
    report("collect_job_info returns success", PMIX_SUCCESS == rc);

    /* the core regression: the returned buffer must not be empty */
    report("returned job-info buffer is non-empty",
           PMIX_SUCCESS == rc && 0 < dbuf.bytes_used);

    /* verify real content: each participating nspace is packed as one byte
     * object whose payload is an inner buffer that begins with the nspace
     * name. Unpack the byte object, load it, and read the leading name. */
    if (PMIX_SUCCESS == rc && 0 < dbuf.bytes_used) {
        pmix_byte_object_t bo;
        pmix_data_buffer_t inner;

        PMIX_BYTE_OBJECT_CONSTRUCT(&bo);
        cnt = 1;
        rc = PMIx_Data_unpack(NULL, &dbuf, &bo, &cnt, PMIX_BYTE_OBJECT);
        if (PMIX_SUCCESS == rc && NULL != bo.bytes && 0 < bo.size) {
            PMIX_DATA_BUFFER_CONSTRUCT(&inner);
            PMIx_Data_load(&inner, &bo);
            cnt = 1;
            PMIx_Data_unpack(NULL, &inner, &unpacked, &cnt, PMIX_STRING);
            report("per-nspace blob carries the namespace name",
                   NULL != unpacked && 0 == strcmp(unpacked, nspace));
            if (NULL != unpacked) {
                free(unpacked);
            }
            PMIX_DATA_BUFFER_DESTRUCT(&inner);
        } else {
            report("per-nspace blob carries the namespace name", 0);
        }
    } else {
        report("per-nspace blob carries the namespace name", 0);
    }

    PMIX_DATA_BUFFER_DESTRUCT(&dbuf);

    fprintf(stdout, "\nResults: %d passed, %d failed\n\n", npass, nfail);

    PMIx_server_finalize();

    return (nfail > 0) ? 1 : 0;
}
