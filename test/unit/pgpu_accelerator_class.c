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
 * An AMD GPU outside the display class is still named to its runtime.
 *
 * The amd component opens only when the node has something for it to do,
 * and it used to ask only about PCI base class 0x03.  An MI350 reports
 * 0x1200, a processing accelerator, so on a node full of them the
 * component never opened: each process was mapped against its GPU and
 * handed the assignment in PMIX_DEVICE_ID, and ROCR_VISIBLE_DEVICES was
 * never written.
 *
 * The test runs against mi350-bcm57608.xml, handed to the server through
 * the documented topo_file hook.  It is a real node read by hwloc 2.15
 * with the RSMI backend - eight MI350s, each with an rsmiN device carrying
 * its AMDUUID, and no AMD function in class 0x03 at all - with its
 * identifying values replaced by synthetic ones.  So the amd component
 * opens only if it asks about the accelerator class, and the module the
 * base calls really is the one selected for this node.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/hwloc/pmix_hwloc.h"
#include "src/include/pmix_globals.h"
#include "src/mca/pgpu/base/base.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_printf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPROCS 2
#define TESTNS "pgpu-accel"

/* the first two GPUs of the fixture, and their (synthetic) AMDUUIDs */
static const char *osnames[NPROCS] = {"rsmi0", "rsmi1"};
static const char *uuids[NPROCS] = {
    "000075a0-0000-1000-8000-000000000001",
    "000075a0-0000-1000-8000-000000000002"
};

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

/* Register a namespace whose rank m was mapped against osnames[m]. */
static pmix_status_t register_nspace(void)
{
    pmix_info_t *info, *pdata;
    pmix_data_array_t *array, *darray;
    pmix_device_t *dev;
    pmix_status_t rc;
    pmix_proc_t p;
    size_t ninfo, n = 0;
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
        pmix_strncpy(info[n].key, PMIX_PROC_INFO_ARRAY, PMIX_MAX_KEYLEN);
        info[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(array, 3, PMIX_INFO);
        info[n].value.data.darray = array;
        pdata = (pmix_info_t *) array->array;

        PMIX_LOAD_PROCID(&p, TESTNS, (pmix_rank_t) m);
        PMIX_INFO_LOAD(&pdata[0], PMIX_PROCID, &p, PMIX_PROC);
        lrank = (uint16_t) m;
        PMIX_INFO_LOAD(&pdata[1], PMIX_LOCAL_RANK, &lrank, PMIX_UINT16);

        PMIX_DATA_ARRAY_CREATE(darray, 1, PMIX_DEVICE);
        dev = (pmix_device_t *) darray->array;
        PMIx_Device_construct(&dev[0]);
        dev[0].osname = strdup(osnames[m]);
        pmix_asprintf(&dev[0].uuid, "gpu://%s::%s", pmix_globals.hostname, osnames[m]);
        dev[0].type = PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC;
        /* the info owns the array from here */
        pmix_strncpy(pdata[2].key, PMIX_DEVICE_ID, PMIX_MAX_KEYLEN);
        pdata[2].value.type = PMIX_DATA_ARRAY;
        pdata[2].value.data.darray = darray;
        ++n;
    }

    /* no callback, so the call blocks and the info may be freed after it */
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
    snprintf(path, sizeof(path), "%s/mi350-bcm57608.xml", PMIX_TEST_TOPO_DIR);
#endif

    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 77;
    }

    /* the premise: no AMD function here is in the display class, so a
     * component asking only about 0x03 has nothing to open on */
    ok(PMIX_ERR_NOT_AVAILABLE
           == pmix_hwloc_check_vendor_baseclass(&pmix_globals.topology, 0x1002, 0x03),
       "the fixture has no AMD display-class function");
    ok(PMIX_SUCCESS
           == pmix_hwloc_check_vendor_baseclass(&pmix_globals.topology, 0x1002, 0x12),
       "the fixture's AMD GPUs are processing accelerators");

    rc = register_nspace();
    ok(PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc, "the namespace registers");
    if (PMIX_SUCCESS != rc && PMIX_OPERATION_SUCCEEDED != rc) {
        goto done;
    }

    /* through the base, so the answer depends on which components opened */
    for (m = 0; m < NPROCS; m++) {
        PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) m);
        env = NULL;
        rc = pmix_pgpu_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds");
        val = envval(env, "ROCR_VISIBLE_DEVICES");
        ok(NULL != val && 0 == strcmp(val, uuids[m]),
           "the rank is given its own accelerator's vendor identity");
        if (NULL != val && 0 != strcmp(val, uuids[m])) {
            fprintf(stderr, "      rank %d: got %s, wanted %s\n", (int) m, val, uuids[m]);
        }
        /* and no other vendor's variable claims it */
        ok(NULL == envval(env, "CUDA_VISIBLE_DEVICES"), "no NVIDIA variable is written");
        ok(NULL == envval(env, "ZE_AFFINITY_MASK"), "no Intel variable is written");
        PMIx_Argv_free(env);
    }

done:
    PMIx_server_finalize();
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
