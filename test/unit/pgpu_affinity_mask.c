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
 * Unit tests for the Intel GPU assignment a pgpu module contributes to a
 * process it is about to fork.
 *
 * Intel is the vendor whose device-selection variable does not accept an
 * identity: ZE_AFFINITY_MASK takes Level Zero device ordinals, and which
 * devices those ordinals name depends on the device hierarchy model the
 * driver enumerated under - a card under COMPOSITE, a tile under FLAT.
 * So an ordinal is only half a statement, and everything here is about
 * the other half:
 *
 *   - the ordinals are the ones this node's own Level Zero driver
 *     reported, read back out of the topology rather than guessed;
 *   - the model they were numbered under is stated alongside them, so a
 *     process whose runtime defaults differently cannot read them to mean
 *     something else;
 *   - and where the process has already been given a model that
 *     contradicts them, NOTHING is written. A wrong value in this
 *     variable does not fail - it silently narrows what the process can
 *     see - so dropping the assignment (the process sees every device it
 *     could see before) is the only safe answer.
 *
 * The fixture is intel-4gpu.xml, an Aurora node cut down to four cards,
 * handed to the server through the documented topo_file hook. It carries
 * Intel display-class PCI functions, so the intel component's own
 * open-time vendor check passes and the module really is selected on a
 * machine that has no GPU; and each function exposes both an OpenCL and a
 * Level Zero view of the same card, with the ordinals on the Level Zero
 * one and the function named by the OpenCL one - so a value only appears
 * if the ordinals were found across the function rather than on the
 * device that names it.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pgpu/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPROCS 2
#define TESTNS "pgpu-affinity"

/* the first two cards of the fixture, and the ordinals their Level Zero
 * driver gave them.  The fixture is COMPOSITE, so a card is one ordinal */
static const char *osnames[NPROCS] = {"opencl0d0", "opencl0d1"};
static const char *masks[NPROCS] = {"0", "1"};

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

/* Register a namespace whose ranks were mapped against devices.  Rank 0
 * gets the first card, rank 1 the second, and rank NPROCS (registered but
 * not mapped) gets none - which is what every ordinary job looks like. */
static pmix_status_t register_nspace(void)
{
    pmix_info_t *info, *pdata;
    pmix_data_array_t *array, *darray;
    pmix_device_t *dev;
    pmix_status_t rc;
    pmix_proc_t p;
    size_t ninfo, n = 0;
    uint32_t nprocs = NPROCS + 1;
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
        PMIX_DATA_ARRAY_CREATE(array, mapped ? 3 : 2, PMIX_INFO);
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
            pmix_asprintf(&dev[0].uuid, "gpu://%s::%s",
                          pmix_globals.hostname, osnames[m]);
            dev[0].type = PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC;
            /* hand the array over rather than PMIX_INFO_LOAD-ing it: the
             * info now owns it */
            pmix_strncpy(pdata[k].key, PMIX_DEVICE_ID, PMIX_MAX_KEYLEN);
            pdata[k].value.type = PMIX_DATA_ARRAY;
            pdata[k].value.data.darray = darray;
        }
        ++n;
    }

    /* No callback, which is what makes this call BLOCK - PMIx substitutes
     * an internal lock when it is given none. */
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

/* Fork rank 0 with a hierarchy model already chosen for it.
 *
 * The contradicting cases print the component's help text, which is the
 * point of them.  A bare server with no client attached also logs
 * "PMIX ERROR: PMIX_ERR_INIT in file plog_stdfd.c" while doing so - that
 * is show_help handing the message to IOF for delivery to a process
 * nobody is listening for, and it says nothing about this code. */
static void with_hierarchy(const char *set, bool expect_mask)
{
    char **env = NULL;
    const char *val;
    pmix_proc_t proc;
    pmix_status_t rc;

    PMIX_LOAD_PROCID(&proc, TESTNS, 0);
    PMIx_Setenv("ZE_FLAT_DEVICE_HIERARCHY", set, true, &env);
    rc = pmix_pgpu_base_setup_fork(&proc, &env);
    /* a module that declines is not an error to the base: this is how it
     * says "nothing to contribute for this process" */
    ok(PMIX_SUCCESS == rc, "a chosen hierarchy is not a fork failure");
    val = envval(env, "ZE_AFFINITY_MASK");
    if (expect_mask) {
        ok(NULL != val && 0 == strcmp(val, masks[0]),
           "an agreeing hierarchy still gets the assignment");
    } else {
        ok(NULL == val, "a contradicting hierarchy drops the assignment");
        if (NULL != val) {
            fprintf(stderr, "      ZE_FLAT_DEVICE_HIERARCHY=%s got mask %s\n",
                    set, val);
        }
    }
    /* the process's own choice is never overwritten either way */
    val = envval(env, "ZE_FLAT_DEVICE_HIERARCHY");
    ok(NULL != val && 0 == strcmp(val, set),
       "the process's own hierarchy choice is left alone");
    PMIx_Argv_free(env);
}

int main(int argc, char **argv)
{
    char **env = NULL;
    const char *val;
    pmix_proc_t proc;
    pmix_status_t rc;
    char path[1024];
    size_t m;

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    snprintf(path, sizeof(path), "%s/intel-4gpu.xml", PMIX_TEST_TOPO_DIR);
#endif

    /* Hand the server the fixture as its own topology.  This is the
     * documented testing hook, and it is what lets the intel component's
     * open-time vendor check pass on a machine with no GPU in it. */
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

    /* each mapped rank is told about its own card, by the ordinal this
     * node's Level Zero driver gave it - which lives on a sibling of the
     * device the assignment names */
    for (m = 0; m < NPROCS; m++) {
        PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) m);
        env = NULL;
        rc = pmix_pgpu_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds");
        val = envval(env, "ZE_AFFINITY_MASK");
        ok(NULL != val && 0 == strcmp(val, masks[m]),
           "the rank is given its own card's ordinal");
        if (NULL != val && 0 != strcmp(val, masks[m])) {
            fprintf(stderr, "      rank %d: got %s, wanted %s\n",
                    (int) m, val, masks[m]);
        }
        /* and told which hierarchy model that ordinal counts in.  Without
         * it the process would take its own runtime's default, which need
         * not be the one this node enumerated under */
        val = envval(env, "ZE_FLAT_DEVICE_HIERARCHY");
        ok(NULL != val && 0 == strcmp(val, "COMPOSITE"),
           "and the model the ordinal is relative to");
        PMIx_Argv_free(env);
    }

    /* two ranks on one node never get the same card named */
    ok(0 != strcmp(masks[0], masks[1]), "the fixture's cards are distinguishable");

    /* a rank that was not mapped against a device gets nothing.  This is
     * the ordinary case for every job that does not map by device, so it
     * has to be silent rather than an error - and in particular it must
     * not get a hierarchy pinned on it either, since there is no
     * assignment for one to qualify */
    PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) NPROCS);
    env = NULL;
    rc = pmix_pgpu_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "an unmapped rank is not an error");
    ok(NULL == envval(env, "ZE_AFFINITY_MASK"),
       "an unmapped rank is told nothing");
    ok(NULL == envval(env, "ZE_FLAT_DEVICE_HIERARCHY"),
       "and has no hierarchy imposed on it");
    PMIx_Argv_free(env);

    /* a process that has already been given the same model keeps its
     * assignment; one given a different model loses it rather than
     * reading the ordinals as tiles.  COMBINED counts as agreeing with
     * FLAT and disagreeing with COMPOSITE: it differs from FLAT only in
     * whether the tiles can be navigated back to their card, and
     * zeDeviceGet reports the same devices in the same order under both */
    with_hierarchy("COMPOSITE", true);
    with_hierarchy("composite", true);
    with_hierarchy("FLAT", false);
    with_hierarchy("COMBINED", false);

    /* the vendor filter: asking for NVIDIA's devices on a machine that
     * has only Intel ones must decline rather than name them */
    PMIX_LOAD_PROCID(&proc, TESTNS, 0);
    env = NULL;
    rc = pmix_pgpu_base_set_visible_devices(&proc, "NVIDIA",
                                            "CUDA_VISIBLE_DEVICES", &env);
    ok(PMIX_ERR_TAKE_NEXT_OPTION == rc, "another vendor's devices are declined");
    ok(NULL == envval(env, "CUDA_VISIBLE_DEVICES"),
       "and no value is written for them");
    PMIx_Argv_free(env);

done:
    PMIx_server_finalize();
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
