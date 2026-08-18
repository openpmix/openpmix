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
 * Unit tests for the device-selection environment a pnet module contributes
 * to a process it is about to fork.
 *
 * The chain under test is the network counterpart of the one
 * pgpu_visible_devices.c covers, and it was missing the same links: the
 * pnet module interface had no per-process hook, so a value that differs
 * per rank had nowhere to be set, and a NIC had no "selector" - no string
 * naming it to the software that would use it.
 *
 * It runs against test-topo2.xml, handed to the server through the
 * documented topo_file hook.  That fixture earns its place here three
 * times over: it carries a Mellanox InfiniBand controller (PCI 15b3 class
 * 0207), so the nvd component's own open-time vendor check passes on a
 * machine with no such card; that controller presents itself as BOTH an
 * OpenFabrics device (mlx5_0) and a network interface (ib0) on one PCI
 * function, with ib0 listed first, so the name the assignment resolves to
 * is the preference rule's answer rather than hwloc's iteration order; and
 * it also carries an Intel ethernet controller (8086 class 0200), which is
 * a network device that must NOT answer to an InfiniBand component.
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

#define NPROCS 2
/* the rank whose assignment is reported in a shape we cannot read */
#define BADRANK (NPROCS + 1)
#define TESTNS "pnet-assigned"

/* rank 0 gets the HCA, rank 1 the ethernet controller.  Both are named the
 * way the mapper names them: by the OS device that won the PCI function. */
static const char *osnames[NPROCS] = {"mlx5_0", "enp67s0"};

/* the PCI families the shipped components open for */
static const pmix_pnet_pcimatch_t ib_match[] = {{0x15b3, 0x207}, {0x10de, 0x207}};
static const pmix_pnet_pcimatch_t opa_match[] = {{0x8086, 0x208}};
static const pmix_pnet_pcimatch_t eth_match[] = {{0x8086, 0x200}};
static const pmix_pnet_pcimatch_t anyintel_match[] = {{0x8086, 0x0}};

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

/* Register a namespace whose ranks were mapped against devices.  Rank
 * NPROCS is registered but not mapped, which is what every ordinary job
 * looks like.  Rank NPROCS+1 carries a PMIX_DEVICE_ID that is an array of
 * *names* rather than of pmix_device_t - the shape a host reading the
 * attribute's documented "(char*)" type would naturally produce for a
 * process with more than one device. */
static pmix_status_t register_nspace(void)
{
    pmix_info_t *info, *pdata;
    pmix_data_array_t *array, *darray;
    pmix_device_t *dev;
    pmix_status_t rc;
    pmix_proc_t p;
    size_t ninfo, n = 0;
    uint32_t nprocs = NPROCS + 2;
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
        bool mapped = (m < NPROCS);
        size_t k = 0;

        pmix_strncpy(info[n].key, PMIX_PROC_INFO_ARRAY, PMIX_MAX_KEYLEN);
        info[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(array, (mapped || BADRANK == m) ? 3 : 2, PMIX_INFO);
        info[n].value.data.darray = array;
        pdata = (pmix_info_t *) array->array;

        PMIX_LOAD_PROCID(&p, TESTNS, (pmix_rank_t) m);
        PMIX_INFO_LOAD(&pdata[k], PMIX_PROCID, &p, PMIX_PROC);
        ++k;
        lrank = (uint16_t) m;
        PMIX_INFO_LOAD(&pdata[k], PMIX_LOCAL_RANK, &lrank, PMIX_UINT16);
        ++k;

        if (mapped) {
            /* always an array of pmix_device_t, even for one device */
            PMIX_DATA_ARRAY_CREATE(darray, 1, PMIX_DEVICE);
            dev = (pmix_device_t *) darray->array;
            PMIx_Device_construct(&dev[0]);
            dev[0].osname = strdup(osnames[m]);
            pmix_asprintf(&dev[0].uuid, "nic://%s::%s",
                          pmix_globals.hostname, osnames[m]);
            dev[0].type = (0 == m) ? PMIX_DEVTYPE_OPENFABRICS : PMIX_DEVTYPE_NETWORK;
            /* hand the array over rather than PMIX_INFO_LOAD-ing it: the
             * info now owns it */
            pmix_strncpy(pdata[k].key, PMIX_DEVICE_ID, PMIX_MAX_KEYLEN);
            pdata[k].value.type = PMIX_DATA_ARRAY;
            pdata[k].value.data.darray = darray;
        } else if (BADRANK == m) {
            char **names = (char **) NULL;

            PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_STRING);
            names = (char **) darray->array;
            names[0] = strdup(osnames[0]);
            names[1] = strdup(osnames[1]);
            pmix_strncpy(pdata[k].key, PMIX_DEVICE_ID, PMIX_MAX_KEYLEN);
            pdata[k].value.type = PMIX_DATA_ARRAY;
            pdata[k].value.data.darray = darray;
        }
        ++n;
    }

    /* No callback, which is what makes this call BLOCK - PMIx substitutes
     * an internal lock when it is given none.  With a callback the call
     * returns immediately and the info array below would be freed while
     * the server was still reading it. */
    PMIX_LOAD_NSPACE(ns, TESTNS);
    rc = PMIx_server_register_nspace(ns, nprocs, info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    return rc;
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

int main(int argc, char **argv)
{
    char **env = NULL;
    char *val = NULL;
    const char *ev;
    pmix_proc_t proc;
    pmix_status_t rc;
    char path[1024];

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    snprintf(path, sizeof(path), "%s/test-topo2.xml", PMIX_TEST_TOPO_DIR);
#endif

    /* Hand the server the fixture as its own topology.  This is the
     * documented testing hook, and it is what lets the nvd component's
     * open-time Mellanox check pass on a machine with no HCA in it. */
    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 77;
    }

    rc = register_nspace();
    /* a synchronous completion reports OPERATION_SUCCEEDED, not SUCCESS */
    ok(PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc,
       "the namespace registers");
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        goto done;
    }

    /* --- the base helper, on its own --- */

    PMIX_LOAD_PROCID(&proc, TESTNS, 0);
    rc = pmix_pnet_base_get_assigned_devices(&proc, ib_match, 2, &val);
    ok(PMIX_SUCCESS == rc && NULL != val && 0 == strcmp(val, "mlx5_0"),
       "an assigned HCA is named by the name its libraries accept");
    if (NULL != val) {
        free(val);
        val = NULL;
    }

    /* the filter is the whole point: an Omni-Path component must not claim
     * a Mellanox card, and an ethernet-class match must not claim an
     * InfiniBand-class one even from the same vendor */
    rc = pmix_pnet_base_get_assigned_devices(&proc, opa_match, 1, &val);
    ok(PMIX_ERR_TAKE_NEXT_OPTION == rc && NULL == val,
       "another fabric's component declines the HCA");

    PMIX_LOAD_PROCID(&proc, TESTNS, 1);
    rc = pmix_pnet_base_get_assigned_devices(&proc, ib_match, 2, &val);
    ok(PMIX_ERR_TAKE_NEXT_OPTION == rc && NULL == val,
       "an InfiniBand component declines an ethernet controller");

    rc = pmix_pnet_base_get_assigned_devices(&proc, eth_match, 1, &val);
    ok(PMIX_SUCCESS == rc && NULL != val && 0 == strcmp(val, "enp67s0"),
       "the matching class claims it");
    if (NULL != val) {
        free(val);
        val = NULL;
    }

    /* a class of zero is "any device from this vendor" */
    rc = pmix_pnet_base_get_assigned_devices(&proc, anyintel_match, 1, &val);
    ok(PMIX_SUCCESS == rc && NULL != val && 0 == strcmp(val, "enp67s0"),
       "a zero class matches any class from that vendor");
    if (NULL != val) {
        free(val);
        val = NULL;
    }

    /* an unmapped rank is the ordinary case and must be silent */
    PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) NPROCS);
    rc = pmix_pnet_base_get_assigned_devices(&proc, ib_match, 2, &val);
    ok(PMIX_ERR_TAKE_NEXT_OPTION == rc && NULL == val,
       "an unmapped rank yields nothing, and is not an error");

    /* An assignment we cannot read is declined, not guessed at.  This rank's
     * PMIX_DEVICE_ID is an array of *names* - the shape a host reading the
     * attribute's documented "(char*)" type would produce for a process with
     * several devices - and reading it as an array of pmix_device_t both
     * runs off the end of the allocation and misreads what is inside it.
     *
     * eth_match is what makes the check bite rather than merely assert.  A
     * pmix_device_t's osname sits where a char*[] holds its *second*
     * element, so the misreading resolves this rank to "enp67s0" and hands
     * back an ethernet controller that was never assigned - a wrong answer,
     * not a crash, which is the harder kind to notice. */
    PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) BADRANK);
    rc = pmix_pnet_base_get_assigned_devices(&proc, eth_match, 1, &val);
    ok(PMIX_ERR_TAKE_NEXT_OPTION == rc && NULL == val,
       "an assignment that is not an array of devices is declined");
    if (NULL != val) {
        free(val);
        val = NULL;
    }

    /* an nspace of "" is a wildcard to PMIX_CHECK_NSPACE, so letting one
     * reach the cache lookup would fork a process with the first job on the
     * list's environment - its transport key included */
    PMIX_LOAD_PROCID(&proc, "", 0);
    env = NULL;
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    ok(PMIX_ERR_BAD_PARAM == rc, "an empty nspace is rejected, not matched");
    PMIx_Argv_free(env);
    env = NULL;

    /* --- the whole fork path, through whichever components selected --- */

    PMIX_LOAD_PROCID(&proc, TESTNS, 0);
    env = NULL;
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank with an HCA");
    ev = envval(env, "NCCL_IB_HCA");
    ok(NULL != ev && 0 == strcmp(ev, "mlx5_0"),
       "NCCL is told the HCA by name");
    /* UCX names a device *port* and matches its value as a glob, so the
     * card is said as "name:*" - and the ":" is what keeps "mlx5_1" from
     * also selecting "mlx5_10" on a node with enough cards */
    ev = envval(env, "UCX_NET_DEVICES");
    ok(NULL != ev && 0 == strcmp(ev, "mlx5_0:*"),
       "UCX is told the card, whatever ports it has");
    PMIx_Argv_free(env);

    /* the ethernet controller is not this component's hardware, so the
     * fork path must leave the rank alone rather than name it */
    PMIX_LOAD_PROCID(&proc, TESTNS, 1);
    env = NULL;
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank with no matching NIC");
    ok(NULL == envval(env, "NCCL_IB_HCA") && NULL == envval(env, "UCX_NET_DEVICES"),
       "and writes nothing for another vendor's device");
    PMIx_Argv_free(env);

    PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) NPROCS);
    env = NULL;
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "an unmapped rank is not an error");
    ok(NULL == envval(env, "NCCL_IB_HCA") && NULL == envval(env, "UCX_NET_DEVICES"),
       "an unmapped rank is told nothing");
    PMIx_Argv_free(env);

done:
    PMIx_server_finalize();
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
