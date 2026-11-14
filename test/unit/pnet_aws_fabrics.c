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
 * The device-selection variables written for a process on an AWS node
 * that carries two RDMA fabrics from two vendors.
 *
 * It runs against aws-p6-b200.xml, a real p6-b200.48xlarge read by hwloc
 * 2.4.1, handed to the server through the documented topo_file hook.  The
 * node has eight EFA devices, which pnet/efa names in FI_EFA_IFACE; two
 * ConnectX-7 InfiniBand ports, which pnet/nvd names to NCCL, UCX and
 * libfabric's verbs provider; and eight B200s with no OS device and no
 * vendor identity.  So both pnet components and pgpu/nvd open, and a
 * process mapped against devices of more than one kind must have each
 * named by its own component and by no other.
 *
 * pnet_rdma_fabrics.c covers EFA beside other vendors' RoCE on a synthetic
 * node; what this adds is the shape AWS actually ships, with EFA's verbs
 * names and the HCA's rdma-core PCI-path names (ibp115s0f0) on one node.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pgpu/base/base.h"
#include "src/mca/pnet/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TESTNS "pnet-aws"

/* What each rank was mapped against, named the way the mapper names a
 * device: by the OS device that won its PCI function, or by its bus id
 * for a GPU that has none. */
static const char *assigned[][3] = {
    {"rdmap79s0", NULL, NULL},               /* 0: one EFA device */
    {"rdmap79s0", "rdmap80s0", NULL},        /* 1: two of them */
    {"ibp115s0f0", NULL, NULL},              /* 2: one InfiniBand port */
    {"rdmap79s0", "ibp115s0f0", NULL},       /* 3: one of each */
    {"enp71s0", NULL, NULL},                 /* 4: an ENA interface */
    {"0000:51:00.0", "rdmap79s0", NULL},     /* 5: a GPU and the EFA beside it */
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
    size_t ninfo, n = 0, ndev, d;
    uint32_t nprocs = NPROCS;
    uint32_t m;
    uint16_t lrank;
    pmix_nspace_t ns;
    bool gpu;

    ninfo = 2 + nprocs;
    PMIX_INFO_CREATE(info, ninfo);
    PMIX_INFO_LOAD(&info[n], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    ++n;
    PMIX_INFO_LOAD(&info[n], PMIX_LOCAL_SIZE, &nprocs, PMIX_UINT32);
    ++n;

    for (m = 0; m < nprocs; m++) {
        pmix_strncpy(info[n].key, PMIX_PROC_INFO_ARRAY, PMIX_MAX_KEYLEN);
        info[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(array, 3, PMIX_INFO);
        info[n].value.data.darray = array;
        pdata = (pmix_info_t *) array->array;

        PMIX_LOAD_PROCID(&p, TESTNS, (pmix_rank_t) m);
        PMIX_INFO_LOAD(&pdata[0], PMIX_PROCID, &p, PMIX_PROC);
        lrank = (uint16_t) m;
        PMIX_INFO_LOAD(&pdata[1], PMIX_LOCAL_RANK, &lrank, PMIX_UINT16);

        ndev = 0;
        while (ndev < 3 && NULL != assigned[m][ndev]) {
            ++ndev;
        }
        PMIX_DATA_ARRAY_CREATE(darray, ndev, PMIX_DEVICE);
        dev = (pmix_device_t *) darray->array;
        for (d = 0; d < ndev; d++) {
            /* a GPU with no OS device is named by its bus id */
            gpu = (0 == strncmp(assigned[m][d], "0000:", 5));
            PMIx_Device_construct(&dev[d]);
            dev[d].osname = strdup(assigned[m][d]);
            pmix_asprintf(&dev[d].uuid, "%s://%s::%s", gpu ? "gpu" : "nic",
                          pmix_globals.hostname, assigned[m][d]);
            dev[d].type = gpu ? (PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC)
                              : (PMIX_DEVTYPE_NETWORK | PMIX_DEVTYPE_OPENFABRICS);
        }
        /* the info owns the array from here */
        pmix_strncpy(pdata[2].key, PMIX_DEVICE_ID, PMIX_MAX_KEYLEN);
        pdata[2].value.type = PMIX_DATA_ARRAY;
        pdata[2].value.data.darray = darray;
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

/* run both frameworks' fork path for one rank */
static char **fork_env(pmix_rank_t rank)
{
    pmix_proc_t proc;
    char **env = NULL;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, TESTNS, rank);
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "pnet setup_fork succeeds");
    rc = pmix_pgpu_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "pgpu setup_fork succeeds");
    return env;
}

/* none of the variables pnet/nvd writes */
static bool no_verbs(char **env)
{
    return (NULL == envval(env, "NCCL_IB_HCA") && NULL == envval(env, "UCX_NET_DEVICES")
            && NULL == envval(env, "FI_VERBS_DEVICE_NAME"));
}

int main(int argc, char **argv)
{
    pmix_status_t rc;
    char path[1024];
    char **env;

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    snprintf(path, sizeof(path), "%s/aws-p6-b200.xml", PMIX_TEST_TOPO_DIR);
#endif
    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 77;
    }

    rc = register_nspace();
    ok(PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc, "the namespace registers");
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        goto done;
    }

    /* EFA: named to libfabric's efa provider, and to none of the verbs
     * libraries, which cannot drive it */
    env = fork_env(0);
    ok(is(envval(env, "FI_EFA_IFACE"), "rdmap79s0"), "efa: libfabric is told the EFA device");
    ok(no_verbs(env), "efa: an EFA device is not named to the verbs libraries");
    PMIx_Argv_free(env);

    env = fork_env(1);
    ok(is(envval(env, "FI_EFA_IFACE"), "rdmap79s0,rdmap80s0"),
       "efa: libfabric is told both EFA devices");
    PMIx_Argv_free(env);

    /* InfiniBand: named to NCCL, UCX and the verbs provider by its
     * rdma-core name, and not as an EFA device */
    env = fork_env(2);
    ok(is(envval(env, "NCCL_IB_HCA"), "ibp115s0f0"), "ib: NCCL is told the HCA");
    ok(is(envval(env, "UCX_NET_DEVICES"), "ibp115s0f0:*"), "ib: UCX is told the HCA");
    ok(is(envval(env, "FI_VERBS_DEVICE_NAME"), "ibp115s0f0"),
       "ib: libfabric's verbs provider is told the HCA");
    ok(NULL == envval(env, "FI_EFA_IFACE"), "ib: an InfiniBand port is not named as an EFA device");
    PMIx_Argv_free(env);

    /* one of each: each component names its own and only its own */
    env = fork_env(3);
    ok(is(envval(env, "FI_EFA_IFACE"), "rdmap79s0"),
       "both: the efa provider is told the EFA device alone");
    ok(is(envval(env, "NCCL_IB_HCA"), "ibp115s0f0") && is(envval(env, "UCX_NET_DEVICES"), "ibp115s0f0:*"),
       "both: NCCL and UCX are told the HCA alone");
    PMIx_Argv_free(env);

    /* ENA is Amazon's too, but a plain interface */
    env = fork_env(4);
    ok(NULL == envval(env, "FI_EFA_IFACE") && no_verbs(env),
       "ena: an ENA interface is named to no RDMA library");
    PMIx_Argv_free(env);

    /* a B200 has no identity here, so CUDA cannot be told which it is and
     * is told nothing - leaving it visible - while the NIC beside it is
     * still named */
    env = fork_env(5);
    ok(NULL == envval(env, "CUDA_VISIBLE_DEVICES"),
       "gpu: a GPU with no identity is not named to CUDA");
    ok(is(envval(env, "FI_EFA_IFACE"), "rdmap79s0"),
       "gpu: the EFA device assigned beside it is still named");
    PMIx_Argv_free(env);

done:
    PMIx_server_finalize();
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
