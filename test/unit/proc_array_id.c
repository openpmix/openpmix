/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 *
 * Unit tests for the identifier of a process-realm info array.
 *
 * PMIX_PROC_INFO_ARRAY lets the host identify the process an array
 * describes with either PMIX_RANK (plus PMIX_NSPACE) or PMIX_PROCID,
 * and places no requirement on where in the array that identifier
 * appears. The datastore used to accept only PMIX_RANK, and only in
 * the first position, rejecting anything else with
 * PMIX_ERR_TYPE_MISMATCH - so a host that followed the documented
 * attribute could not register its job.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/gds/base/base.h"
#include "src/mca/gds/gds.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int npass = 0;
static int nfail = 0;

static void report(const char *name, int passed)
{
    if (passed) {
        fprintf(stdout, "  PASS: %s\n", name);
        npass++;
    } else {
        fprintf(stdout, "  FAIL: %s\n", name);
        nfail++;
    }
}

/* ------------------------------------------------------------------ */
/* the identifier scan itself                                          */
/* ------------------------------------------------------------------ */
static void test_scan(void)
{
    pmix_info_t array[3];
    pmix_proc_t p;
    pmix_status_t rc;
    pmix_rank_t rank = PMIX_RANK_UNDEF;
    size_t idpos = SIZE_MAX;

    fprintf(stdout, "\n-- identifier scan --\n");

    /* the classic form: a rank in the first position */
    PMIX_INFO_LOAD(&array[0], PMIX_RANK, &(pmix_rank_t){3}, PMIX_PROC_RANK);
    PMIX_INFO_LOAD(&array[1], PMIX_LOCAL_RANK, &(uint16_t){1}, PMIX_UINT16);
    rc = pmix_gds_base_proc_array_id(array, 2, &rank, &idpos);
    report("rank in the first position is found",
           PMIX_SUCCESS == rc && 3 == rank && 0 == idpos);
    PMIX_INFO_DESTRUCT(&array[0]);
    PMIX_INFO_DESTRUCT(&array[1]);

    /* the same identifier, further down the array */
    PMIX_INFO_LOAD(&array[0], PMIX_LOCAL_RANK, &(uint16_t){1}, PMIX_UINT16);
    PMIX_INFO_LOAD(&array[1], PMIX_NODEID, &(uint32_t){0}, PMIX_UINT32);
    PMIX_INFO_LOAD(&array[2], PMIX_RANK, &(pmix_rank_t){7}, PMIX_PROC_RANK);
    rc = pmix_gds_base_proc_array_id(array, 3, &rank, &idpos);
    report("rank in a later position is found",
           PMIX_SUCCESS == rc && 7 == rank && 2 == idpos);
    PMIX_INFO_DESTRUCT(&array[0]);
    PMIX_INFO_DESTRUCT(&array[1]);
    PMIX_INFO_DESTRUCT(&array[2]);

    /* identified by procid instead of rank */
    PMIX_LOAD_PROCID(&p, "myjob", 5);
    PMIX_INFO_LOAD(&array[0], PMIX_LOCAL_RANK, &(uint16_t){1}, PMIX_UINT16);
    PMIX_INFO_LOAD(&array[1], PMIX_PROCID, &p, PMIX_PROC);
    rc = pmix_gds_base_proc_array_id(array, 2, &rank, &idpos);
    report("procid is accepted as the identifier",
           PMIX_SUCCESS == rc && 5 == rank && 1 == idpos);
    PMIX_INFO_DESTRUCT(&array[0]);
    PMIX_INFO_DESTRUCT(&array[1]);

    /* both present - the explicit rank is the one to use */
    PMIX_LOAD_PROCID(&p, "myjob", 5);
    PMIX_INFO_LOAD(&array[0], PMIX_PROCID, &p, PMIX_PROC);
    PMIX_INFO_LOAD(&array[1], PMIX_RANK, &(pmix_rank_t){9}, PMIX_PROC_RANK);
    rc = pmix_gds_base_proc_array_id(array, 2, &rank, &idpos);
    report("an explicit rank wins over a procid",
           PMIX_SUCCESS == rc && 9 == rank && 1 == idpos);
    PMIX_INFO_DESTRUCT(&array[0]);
    PMIX_INFO_DESTRUCT(&array[1]);

    /* nothing that says who this is */
    PMIX_INFO_LOAD(&array[0], PMIX_LOCAL_RANK, &(uint16_t){1}, PMIX_UINT16);
    PMIX_INFO_LOAD(&array[1], PMIX_NODEID, &(uint32_t){0}, PMIX_UINT32);
    rc = pmix_gds_base_proc_array_id(array, 2, &rank, &idpos);
    report("an unidentified array is rejected", PMIX_ERR_TYPE_MISMATCH == rc);
    PMIX_INFO_DESTRUCT(&array[0]);
    PMIX_INFO_DESTRUCT(&array[1]);

    /* the right key carrying the wrong type */
    PMIX_INFO_LOAD(&array[0], PMIX_RANK, &(uint16_t){2}, PMIX_UINT16);
    rc = pmix_gds_base_proc_array_id(array, 1, &rank, &idpos);
    report("a rank of the wrong type is rejected", PMIX_ERR_TYPE_MISMATCH == rc);
    PMIX_INFO_DESTRUCT(&array[0]);

    /* nothing to scan */
    rc = pmix_gds_base_proc_array_id(NULL, 0, &rank, &idpos);
    report("a missing array is rejected", PMIX_ERR_BAD_PARAM == rc);
}

/* ------------------------------------------------------------------ */
/* the same thing through a real namespace registration                */
/* ------------------------------------------------------------------ */

/* Build a job whose per-proc arrays are identified the way the
 * attribute allows but the datastore used to refuse: by PMIX_PROCID,
 * and not in the first position. */
static pmix_status_t register_job(const char *nspace, int nprocs)
{
    char *regex = NULL, *ppn = NULL, *rks, **agg = NULL;
    char tmp[64];
    pmix_info_t *info, *iptr, *pdata;
    pmix_data_array_t *array;
    pmix_proc_t p;
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

    ninfo = 1 + (size_t) nnodes + (size_t) nprocs;
    PMIX_INFO_CREATE(info, ninfo);

    n = 0;
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

    pmix_strncpy(info[n].key, PMIX_NODE_INFO_ARRAY, PMIX_MAX_KEYLEN);
    info[n].value.type = PMIX_DATA_ARRAY;
    PMIX_DATA_ARRAY_CREATE(info[n].value.data.darray, 3, PMIX_INFO);
    iptr = (pmix_info_t *) info[n].value.data.darray->array;
    PMIX_INFO_LOAD(&iptr[0], PMIX_HOSTNAME, pmix_globals.hostname, PMIX_STRING);
    m = 0;
    PMIX_INFO_LOAD(&iptr[1], PMIX_NODEID, &m, PMIX_UINT32);
    PMIX_INFO_LOAD(&iptr[2], PMIX_NODE_SIZE, &nprocs, PMIX_UINT32);
    ++n;

    /* per-proc data, identified by a procid that is deliberately not
     * the first element */
    for (m = 0; m < nprocs; m++) {
        pmix_strncpy(info[n].key, PMIX_PROC_DATA, PMIX_MAX_KEYLEN);
        info[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(array, 3, PMIX_INFO);
        info[n].value.data.darray = array;
        pdata = (pmix_info_t *) array->array;
        k = 0;
        pmix_strncpy(pdata[k].key, PMIX_LOCAL_RANK, PMIX_MAX_KEYLEN);
        pdata[k].value.type = PMIX_UINT16;
        pdata[k].value.data.uint16 = (uint16_t) (100 + m);
        ++k;
        PMIX_LOAD_PROCID(&p, nspace, (pmix_rank_t) m);
        PMIX_INFO_LOAD(&pdata[k], PMIX_PROCID, &p, PMIX_PROC);
        ++k;
        pmix_strncpy(pdata[k].key, PMIX_NODEID, PMIX_MAX_KEYLEN);
        pdata[k].value.type = PMIX_UINT32;
        pdata[k].value.data.uint32 = 0;
        ++n;
    }

    PMIX_LOAD_NSPACE(ns, nspace);
    rc = PMIx_server_register_nspace(ns, nprocs, info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    return rc;
}

/* fetch one key for one rank out of the local datastore */
static pmix_status_t fetch_one(const char *nspace, pmix_rank_t rank,
                               const char *key, pmix_value_t *result)
{
    pmix_cb_t cb;
    pmix_proc_t proc;
    pmix_kval_t *kv;
    pmix_status_t rc;

    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    PMIX_LOAD_PROCID(&proc, nspace, rank);
    cb.proc = &proc;
    cb.key = (char *) key;
    cb.scope = PMIX_INTERNAL;
    cb.copy = false;
    cb.info = NULL;
    cb.ninfo = 0;

    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    if (PMIX_SUCCESS == rc && NULL != result) {
        kv = (pmix_kval_t *) pmix_list_get_first(&cb.kvs);
        if (NULL != kv && NULL != kv->value) {
            PMIX_VALUE_XFER_DIRECT(rc, result, kv->value);
        } else {
            rc = PMIX_ERR_NOT_FOUND;
        }
    }
    cb.proc = NULL;
    cb.key = NULL;
    PMIX_DESTRUCT(&cb);
    return rc;
}

static void test_registration(void)
{
    const char *nspace = "procid-identified";
    pmix_status_t rc;
    pmix_value_t val;

    fprintf(stdout, "\n-- namespace registration --\n");

    rc = register_job(nspace, 2);
    report("a procid-identified proc array registers",
           PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc);
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        fprintf(stdout, "    register_nspace said: %s\n", PMIx_Error_string(rc));
        return;
    }

    /* the values either side of the identifier must have been stored,
     * and attributed to the rank the procid named */
    PMIX_VALUE_CONSTRUCT(&val);
    rc = fetch_one(nspace, 1, PMIX_LOCAL_RANK, &val);
    report("a value ahead of the identifier is stored for the right rank",
           PMIX_SUCCESS == rc && PMIX_UINT16 == val.type && 101 == val.data.uint16);
    PMIX_VALUE_DESTRUCT(&val);

    PMIX_VALUE_CONSTRUCT(&val);
    rc = fetch_one(nspace, 0, PMIX_LOCAL_RANK, &val);
    report("the other rank got its own value",
           PMIX_SUCCESS == rc && PMIX_UINT16 == val.type && 100 == val.data.uint16);
    PMIX_VALUE_DESTRUCT(&val);

    /* the identifier names the array rather than being data belonging
     * to the process, so it is consumed rather than stored */
    rc = fetch_one(nspace, 1, PMIX_PROCID, NULL);
    report("the identifier itself is not stored as a key", PMIX_SUCCESS != rc);
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    static pmix_server_module_t mymodule = {0};
    PMIX_HIDE_UNUSED_PARAMS(argc, argv);

    rc = PMIx_server_init(&mymodule, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 1;
    }

    fprintf(stdout, "\n=== proc-array identifier unit tests ===\n");

    test_scan();
    test_registration();

    fprintf(stdout, "\n=== %d passed, %d failed ===\n\n", npass, nfail);

    PMIx_server_finalize();
    return (0 == nfail) ? 0 : 1;
}
