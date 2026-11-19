/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2026      Nanook Consulting  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * $HEADER$
 */

/*
 * The pnet opa component on a CN5000 node.
 *
 * It runs against cn5k-hfi.xml, a real CN5000 node with its identifying
 * values replaced (see test/topologies/Makefile.am).  Two things about that
 * node are what this covers, and neither is in opa-hfi.xml:
 *
 * - The HFI reports Cornelis's own PCI vendor id, 434e, not the Intel id
 *   OPA100 carries.  A component that looked for Intel's id alone never
 *   opened here, so a CN5000 job got neither its HFI named to OPX nor an
 *   OPX job key.  That is a failure here, not a skip: the fixture has the
 *   HFI, so a component that does not open is the defect.
 *
 * - The HFI's PCI function carries two network interfaces (ibp161s0,
 *   ibp161s0d1) beside its OpenFabrics device (hfi1_0), and a host may
 *   name the device by any of them.  OPX knows only the hfi1_<N> name, so
 *   either way the process must be told unit 0.
 *
 * The node also has two Mellanox InfiniBand ports, which are nvd's and
 * not ours.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/opa/pnet_opa.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TESTNS "pnet-opa-cn5k"
#define NPROCS 3
#define OPXKEY "FI_OPX_UUID"
#define OPXSEL "FI_OPX_HFI_SELECT"

/* rank 0 is named the HFI by its OpenFabrics device, rank 1 by one of its
 * network interfaces, rank 2 a Mellanox port */
static const struct {
    const char *osname;
    pmix_device_type_t type;
} assigned[NPROCS] = {{"hfi1_0", PMIX_DEVTYPE_OPENFABRICS},
                      {"ibp161s0", PMIX_DEVTYPE_NETWORK},
                      {"mlx5_0", PMIX_DEVTYPE_OPENFABRICS}};

static int failures = 0;
static int checks = 0;

static void ok(bool cond, const char *what)
{
    ++checks;
    if (!cond) {
        ++failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

/* the value of "var" in env, or NULL */
static const char *envval(char **env, const char *var)
{
    size_t len = strlen(var);
    int i;

    for (i = 0; NULL != env && NULL != env[i]; i++) {
        if (0 == strncmp(env[i], var, len) && '=' == env[i][len]) {
            return &env[i][len + 1];
        }
    }
    return NULL;
}

/* Ask for the job's seckeys and deliver the blob the way a compute-node
 * daemon would receive it.  Reports whether opa contributed one. */
static bool allocate_and_deliver(void)
{
    pmix_info_t directive, *blobs;
    pmix_list_t ilist;
    pmix_kval_t *kv;
    size_t n, nblobs;
    bool flag = true, ours = false;
    pmix_status_t rc;

    PMIX_INFO_LOAD(&directive, PMIX_SETUP_APP_ALL, &flag, PMIX_BOOL);
    PMIX_CONSTRUCT(&ilist, pmix_list_t);
    rc = pmix_pnet.allocate(TESTNS, &directive, 1, &ilist);
    PMIX_INFO_DESTRUCT(&directive);
    nblobs = pmix_list_get_size(&ilist);
    if (PMIX_SUCCESS != rc || 0 == nblobs) {
        PMIX_LIST_DESTRUCT(&ilist);
        return false;
    }
    PMIX_INFO_CREATE(blobs, nblobs);
    n = 0;
    PMIX_LIST_FOREACH (kv, &ilist, pmix_kval_t) {
        PMIX_LOAD_KEY(blobs[n].key, kv->key);
        PMIx_Value_xfer(&blobs[n].value, kv->value);
        if (0 == strcmp(kv->key, PMIX_PNET_OPA_BLOB)) {
            ours = true;
        }
        ++n;
    }
    PMIX_LIST_DESTRUCT(&ilist);
    rc = pmix_pnet.setup_local_network(TESTNS, blobs, nblobs);
    PMIX_INFO_FREE(blobs, nblobs);
    return ours && PMIX_SUCCESS == rc;
}

static pmix_status_t register_nspace(void)
{
    pmix_info_t *info, *pdata;
    pmix_data_array_t *array, *darray;
    pmix_device_t *dev;
    pmix_status_t rc;
    pmix_proc_t p;
    size_t ninfo, n = 0, k;
    uint32_t nprocs = NPROCS, m;
    uint16_t lrank;
    pmix_nspace_t ns;

    ninfo = 2 + nprocs;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[n], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    ++n;
    PMIX_INFO_LOAD(&info[n], PMIX_LOCAL_SIZE, &nprocs, PMIX_UINT32);
    ++n;

    for (m = 0; m < nprocs; m++) {
        k = 0;
        pmix_strncpy(info[n].key, PMIX_PROC_INFO_ARRAY, PMIX_MAX_KEYLEN);
        info[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(array, 3, PMIX_INFO);
        info[n].value.data.darray = array;
        pdata = (pmix_info_t *) array->array;

        PMIX_LOAD_PROCID(&p, TESTNS, (pmix_rank_t) m);
        PMIX_INFO_LOAD(&pdata[k], PMIX_PROCID, &p, PMIX_PROC);
        ++k;
        lrank = (uint16_t) m;
        PMIX_INFO_LOAD(&pdata[k], PMIX_LOCAL_RANK, &lrank, PMIX_UINT16);
        ++k;

        PMIX_DATA_ARRAY_CREATE(darray, 1, PMIX_DEVICE);
        dev = (pmix_device_t *) darray->array;
        PMIx_Device_construct(&dev[0]);
        dev[0].osname = strdup(assigned[m].osname);
        pmix_asprintf(&dev[0].uuid, "nic://%s::%s", pmix_globals.hostname, assigned[m].osname);
        dev[0].type = assigned[m].type;
        /* hand the array over: the info now owns it */
        pmix_strncpy(pdata[k].key, PMIX_DEVICE_ID, PMIX_MAX_KEYLEN);
        pdata[k].value.type = PMIX_DATA_ARRAY;
        pdata[k].value.data.darray = darray;
        ++n;
    }

    /* No callback, which is what makes this call BLOCK */
    PMIX_LOAD_NSPACE(ns, TESTNS);
    rc = PMIx_server_register_nspace(ns, nprocs, info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    return rc;
}

/* the environment rank would be forked with; the caller frees it */
static char **fork_env(pmix_rank_t rank, pmix_status_t *rc)
{
    char **env = NULL;
    pmix_proc_t proc;

    PMIX_LOAD_PROCID(&proc, TESTNS, rank);
    *rc = pmix_pnet_base_setup_fork(&proc, &env);
    return env;
}

int main(int argc, char **argv)
{
    char path[1024];
    char **env;
    const char *ev;
    pmix_status_t rc;

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    snprintf(path, sizeof(path), "%s/cn5k-hfi.xml", PMIX_TEST_TOPO_DIR);
#endif

    /* the documented testing hook: the server takes the fixture as its own
     * topology */
    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 77;
    }

    ok(allocate_and_deliver(), "opa opens for a CN5000 HFI and contributes its blob");

    rc = register_nspace();
    ok(PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc, "the namespace registers");

    env = fork_env(0, &rc);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank on hfi1_0");
    ev = envval(env, OPXSEL);
    ok(NULL != ev && 0 == strcmp(ev, "0"), "OPX is told unit 0");
    ok(NULL == envval(env, "PSM3_NIC"), "and PSM3, which is not an Omni-Path library, is told nothing");
    ok(NULL != envval(env, OPXKEY), "and the job's OPX key is set");
    PMIx_Argv_free(env);

    /* named by a network interface on the same function: the device is the
     * HFI all the same, and OPX knows it only as hfi1_0 */
    env = fork_env(1, &rc);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank named by ibp161s0");
    ev = envval(env, OPXSEL);
    ok(NULL != ev && 0 == strcmp(ev, "0"),
       "an HFI named by its network interface is still unit 0 to OPX");
    PMIx_Argv_free(env);

    /* a Mellanox port on the same node is nvd's, not ours */
    env = fork_env(2, &rc);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank on mlx5_0");
    ok(NULL == envval(env, OPXSEL), "and names no HFI for a Mellanox port");
    ok(NULL != envval(env, OPXKEY), "but the job's OPX key is set all the same");
    PMIx_Argv_free(env);

    PMIx_server_finalize();
    fprintf(stderr, "%s: %d checks, %d failures\n", (0 == failures) ? "PASS" : "FAIL", checks,
            failures);
    return (0 == failures) ? 0 : 1;
}
