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
 * The device-selection variables the pnet components write for RDMA
 * fabrics other than InfiniBand: RoCE from NVIDIA, Broadcom and Intel,
 * Amazon's EFA, and HPE's Slingshot.
 *
 * It runs against rdma-fabrics.xml, a synthetic node carrying one function
 * of each, handed to the server through the documented topo_file hook so
 * every component's open-time check passes on a machine with none of the
 * hardware.  pnet_assigned_devices.c covers the InfiniBand case and the
 * base helper's contract against test-topo2; this file is about telling
 * these fabrics apart, which is where they are easy to get wrong:
 *
 *  - every RoCE adapter reports the ordinary ethernet class 0x200, as
 *    every plain ethernet port from the same vendor does, so the match has
 *    to require the OpenFabrics device hwloc found on the function;
 *  - an Intel irdma device may run iWARP, which the topology cannot see,
 *    so nvd reads its node type from a captured sysfs tree (see
 *    pnet_sysfs.h) - irdma0 is a channel adapter there, irdma1 is not, and
 *    mlx5_2 has no entry at all;
 *  - libfabric's verbs provider matches FI_VERBS_DEVICE_NAME by prefix, and
 *    the node carries bnxt_re1 and bnxt_re10.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pnet/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pnet_sysfs.h"

#define TESTNS "pnet-rdma"

/* What each rank was mapped against, named the way the mapper names a
 * device: by the OS device that won its PCI function. */
static const char *assigned[][3] = {
    {"bnxt_re1", NULL, NULL},       /* 0: Broadcom RoCE; bnxt_re10 also exists */
    {"bnxt_re1", "bnxt_re10", NULL},/* 1: both of them */
    {"irdma0", NULL, NULL},         /* 2: Intel E810 in RoCE mode */
    {"irdma1", NULL, NULL},         /* 3: Intel E810 in iWARP mode */
    {"irdma0", "irdma1", NULL},     /* 4: one of each */
    {"enp35s0", NULL, NULL},        /* 5: a plain Intel ethernet port */
    {"mlx5_1", NULL, NULL},         /* 6: NVIDIA ConnectX in ethernet mode */
    {"mlx5_2", NULL, NULL},         /* 7: the same, with no node type to read */
    {"rdmap49s0", NULL, NULL},      /* 8: EFA */
    {"ens50", NULL, NULL},          /* 9: ENA, beside it from the same vendor */
    {"cxi0", NULL, NULL},           /* 10: Slingshot, as hwloc 2.14 names it */
    {"hsn1", NULL, NULL},           /* 11: Slingshot, as an older hwloc does */
    {"hsn0", NULL, NULL},           /* 12: Slingshot, asked for by its interface */
};
#define NPROCS ((uint32_t) (sizeof(assigned) / sizeof(assigned[0])))

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

static pmix_status_t register_nspace(void)
{
    pmix_info_t *info, *pdata;
    pmix_data_array_t *array, *darray;
    pmix_device_t *dev;
    pmix_status_t rc;
    pmix_proc_t p;
    size_t ninfo, n = 0, k, ndev, d;
    uint32_t nprocs = NPROCS;
    uint32_t m;
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

        ndev = 0;
        while (ndev < 3 && NULL != assigned[m][ndev]) {
            ++ndev;
        }
        PMIX_DATA_ARRAY_CREATE(darray, ndev, PMIX_DEVICE);
        dev = (pmix_device_t *) darray->array;
        for (d = 0; d < ndev; d++) {
            PMIx_Device_construct(&dev[d]);
            dev[d].osname = strdup(assigned[m][d]);
            pmix_asprintf(&dev[d].uuid, "nic://%s::%s", pmix_globals.hostname, assigned[m][d]);
            dev[d].type = PMIX_DEVTYPE_NETWORK;
        }
        pmix_strncpy(pdata[k].key, PMIX_DEVICE_ID, PMIX_MAX_KEYLEN);
        pdata[k].value.type = PMIX_DATA_ARRAY;
        pdata[k].value.data.darray = darray;
        ++n;
    }

    /* no callback, so the call blocks - see pnet_assigned_devices.c */
    PMIX_LOAD_NSPACE(ns, TESTNS);
    rc = PMIx_server_register_nspace(ns, nprocs, info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    return rc;
}

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

static bool is(const char *val, const char *want)
{
    return (NULL != val && 0 == strcmp(val, want));
}

/* run the whole fork path for one rank */
static char **fork_env(pmix_rank_t rank)
{
    pmix_proc_t proc;
    char **env = NULL;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, TESTNS, rank);
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds");
    return env;
}

/* none of the verbs variables */
static bool no_verbs(char **env)
{
    return (NULL == envval(env, "NCCL_IB_HCA") && NULL == envval(env, "UCX_NET_DEVICES")
            && NULL == envval(env, "FI_VERBS_DEVICE_NAME"));
}

static void test_roce(void)
{
    char **env;

    /* Broadcom: the verbs libraries take bnxt_re1 as they take an
     * InfiniBand HCA.  FI_VERBS_DEVICE_NAME would also select bnxt_re10,
     * which this rank was not given, so it is left alone. */
    env = fork_env(0);
    ok(is(envval(env, "NCCL_IB_HCA"), "bnxt_re1"), "roce: NCCL is told the Broadcom adapter");
    ok(is(envval(env, "UCX_NET_DEVICES"), "bnxt_re1:*"), "roce: UCX is told the Broadcom adapter");
    ok(NULL == envval(env, "FI_VERBS_DEVICE_NAME"),
       "roce: a verbs name that is a prefix of another device's is not written");
    PMIx_Argv_free(env);

    /* given both, the prefix selects nothing it was not given */
    env = fork_env(1);
    ok(is(envval(env, "NCCL_IB_HCA"), "bnxt_re1,bnxt_re10"), "roce: NCCL is told both adapters");
    ok(is(envval(env, "FI_VERBS_DEVICE_NAME"), "bnxt_re1,bnxt_re10"),
       "roce: a prefix that only selects assigned devices is written");
    PMIx_Argv_free(env);

    /* Intel in RoCE mode: all three */
    env = fork_env(2);
    ok(is(envval(env, "NCCL_IB_HCA"), "irdma0"), "roce: NCCL is told the RoCE irdma device");
    ok(is(envval(env, "UCX_NET_DEVICES"), "irdma0:*"), "roce: UCX is told it");
    ok(is(envval(env, "FI_VERBS_DEVICE_NAME"), "irdma0"), "roce: libfabric verbs is told it");
    PMIx_Argv_free(env);

    /* Intel in iWARP mode: UCX and NCCL cannot drive it, and nothing in
     * the topology says so - only its node type does */
    env = fork_env(3);
    ok(no_verbs(env), "roce: an iWARP device is named to none of the verbs libraries");
    PMIx_Argv_free(env);

    env = fork_env(4);
    ok(is(envval(env, "NCCL_IB_HCA"), "irdma0") && is(envval(env, "UCX_NET_DEVICES"), "irdma0:*"),
       "roce: given one of each, only the RoCE device is named");
    PMIx_Argv_free(env);

    /* a plain ethernet port from a vendor that also makes RoCE adapters */
    env = fork_env(5);
    ok(no_verbs(env), "roce: a plain ethernet port is not named as an RDMA adapter");
    PMIx_Argv_free(env);

    /* NVIDIA ConnectX in ethernet mode */
    env = fork_env(6);
    ok(is(envval(env, "NCCL_IB_HCA"), "mlx5_1") && is(envval(env, "UCX_NET_DEVICES"), "mlx5_1:*")
       && is(envval(env, "FI_VERBS_DEVICE_NAME"), "mlx5_1"),
       "roce: a ConnectX in ethernet mode is named like an InfiniBand HCA");
    PMIx_Argv_free(env);

    /* a device whose node type cannot be read is left out, not assumed */
    env = fork_env(7);
    ok(no_verbs(env), "roce: a device with no readable node type is not named");
    PMIx_Argv_free(env);

    /* EFA is an OpenFabrics device none of the verbs libraries drive, and
     * ENA is a plain interface from the same vendor */
    env = fork_env(8);
    ok(no_verbs(env), "roce: an EFA device is not named to the verbs libraries");
    PMIx_Argv_free(env);
    env = fork_env(9);
    ok(no_verbs(env), "roce: an ENA interface is not named to the verbs libraries");
    PMIx_Argv_free(env);
}

int main(int argc, char **argv)
{
    pnet_sysfs_t sysfs;
    pmix_status_t rc;
    char path[1024];

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    snprintf(path, sizeof(path), "%s/rdma-fabrics.xml", PMIX_TEST_TOPO_DIR);
#endif
    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    /* the node types a kernel would report for these devices; mlx5_2 is
     * deliberately absent */
    if (!pnet_sysfs_init(&sysfs) || !pnet_sysfs_add(&sysfs, "bnxt_re1", "1: CA")
        || !pnet_sysfs_add(&sysfs, "bnxt_re10", "1: CA")
        || !pnet_sysfs_add(&sysfs, "irdma0", "1: CA")
        || !pnet_sysfs_add(&sysfs, "irdma1", "4: RNIC")
        || !pnet_sysfs_add(&sysfs, "mlx5_1", "1: CA")
        || !pnet_sysfs_add(&sysfs, "rdmap49s0", "1: CA")) {
        fprintf(stderr, "SKIP: cannot build a sysfs tree\n");
        pnet_sysfs_remove(&sysfs);
        return 77;
    }
    setenv("PMIX_MCA_pnet_nvd_sysfs_root", sysfs.root, 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        pnet_sysfs_remove(&sysfs);
        return 77;
    }

    rc = register_nspace();
    ok(PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc, "the namespace registers");
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
        test_roce();
    }

    PMIx_server_finalize();
    pnet_sysfs_remove(&sysfs);
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
