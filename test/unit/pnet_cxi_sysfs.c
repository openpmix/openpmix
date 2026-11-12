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
 * Naming a Slingshot NIC to libfabric's CXI provider on a node whose hwloc
 * does not report the CXI device.
 *
 * It runs against slingshot.xml, the topology of a real HPE Cray EX235n
 * node taken with hwloc 2.10.0: four Cassini functions, each reported only
 * as its hsnN interface.  FI_CXI_DEVICE_NAME accepts only cxiN, so the cxi
 * component looks the CXI device up where hwloc 2.14 itself finds it,
 * <root>/class/net/<hsnN>/device/cxi/, in a captured tree built by
 * pnet_sysfs.h:
 *
 *   hsn0  ->  cxi0                    one entry: resolved
 *   hsn1  ->  (no directory)          declined
 *   hsn2  ->  cxi2, cxi7              more than one: declined
 *   hsn3  ->  cxi5, plus a non-CXI    the non-CXI entry is ignored
 *
 * hsn3's CXI device is cxi5, and hsn1 has none at all, so a lookup that
 * derived the name from the interface's number would be caught.
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

#define TESTNS "pnet-cxi"

static const char *assigned[][3] = {
    {"hsn0", NULL, NULL},           /* 0: resolved to cxi0 */
    {"hsn1", NULL, NULL},           /* 1: no cxi directory */
    {"hsn2", NULL, NULL},           /* 2: two CXI devices listed */
    {"hsn3", NULL, NULL},           /* 3: one CXI device, cxi5, beside another entry */
    {"hsn0", "hsn3", NULL},         /* 4: two NICs, both resolved */
    {"hsn0", "hsn1", NULL},         /* 5: two NICs, one not resolved */
    {"nmn0", NULL, NULL},           /* 6: the Intel management port */
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

int main(int argc, char **argv)
{
    pnet_sysfs_t sysfs;
    pmix_status_t rc;
    char path[1024];
    char **env;

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    snprintf(path, sizeof(path), "%s/slingshot.xml", PMIX_TEST_TOPO_DIR);
#endif
    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    if (!pnet_sysfs_init(&sysfs)
        || !pnet_sysfs_add_dir(&sysfs, "class/net/hsn0/device/cxi/cxi0")
        || !pnet_sysfs_add_dir(&sysfs, "class/net/hsn1/device")
        || !pnet_sysfs_add_dir(&sysfs, "class/net/hsn2/device/cxi/cxi2")
        || !pnet_sysfs_add_dir(&sysfs, "class/net/hsn2/device/cxi/cxi7")
        || !pnet_sysfs_add_dir(&sysfs, "class/net/hsn3/device/cxi/cxi5")
        || !pnet_sysfs_add_dir(&sysfs, "class/net/hsn3/device/cxi/subsystem")) {
        fprintf(stderr, "SKIP: cannot build a sysfs tree\n");
        pnet_sysfs_remove(&sysfs);
        return 77;
    }
    setenv("PMIX_MCA_pnet_cxi_sysfs_root", sysfs.root, 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        pnet_sysfs_remove(&sysfs);
        return 77;
    }

    rc = register_nspace();
    ok(PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc, "the namespace registers");
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
        env = fork_env(0);
        ok(is(envval(env, "FI_CXI_DEVICE_NAME"), "cxi0"),
           "an interface's CXI device is found in sysfs");
        PMIx_Argv_free(env);

        env = fork_env(1);
        ok(NULL == envval(env, "FI_CXI_DEVICE_NAME"),
           "an interface with no CXI directory is not named");
        PMIx_Argv_free(env);

        env = fork_env(2);
        ok(NULL == envval(env, "FI_CXI_DEVICE_NAME"),
           "an interface listing two CXI devices is not named");
        PMIx_Argv_free(env);

        env = fork_env(3);
        ok(is(envval(env, "FI_CXI_DEVICE_NAME"), "cxi5"),
           "the CXI device is the one listed, not one numbered like the interface");
        PMIx_Argv_free(env);

        env = fork_env(4);
        ok(is(envval(env, "FI_CXI_DEVICE_NAME"), "cxi0,cxi5"),
           "two resolved interfaces are both named");
        PMIx_Argv_free(env);

        env = fork_env(5);
        ok(NULL == envval(env, "FI_CXI_DEVICE_NAME"),
           "one unresolved interface leaves the variable unset, not short");
        PMIx_Argv_free(env);

        env = fork_env(6);
        ok(NULL == envval(env, "FI_CXI_DEVICE_NAME"),
           "the management port is not a Slingshot NIC");
        PMIx_Argv_free(env);
    }

    PMIx_server_finalize();
    pnet_sysfs_remove(&sysfs);
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
