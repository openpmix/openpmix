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
 * An AMD GPU outside the display class is still named to its runtime, in
 * the form that runtime reads.
 *
 * The amd component opens only when the node has something for it to do,
 * and it used to ask only about PCI base class 0x03.  An MI350 reports
 * 0x1200, a processing accelerator, so on a node full of them the
 * component never opened and ROCR_VISIBLE_DEVICES was never written.
 *
 * Once written, the value has to be one ROCm accepts.  ROCm's filter
 * takes an entry beginning with 'G' as a prefix of "GPU-" and the 16-digit
 * unique id, parses anything else as an index, and stops at the first
 * entry that matches nothing; hwloc's own spelling of the id is neither,
 * so a process given it saw no GPU.  The value is checked here in ROCm's
 * form.  And a process that was given several GPUs, one of which cannot
 * be named, is told nothing rather than a list that hides that one.
 *
 * The test runs against mi350-bcm57608.xml, handed to the server through
 * the documented topo_file hook.  It is a real node read by hwloc 2.15
 * with AMD SMI - eight MI350s, each with an rsmiN device carrying its
 * AMDUUID, and no AMD function in class 0x03 at all - with its
 * identifying values replaced by synthetic ones: rsmiN's id is N + 1.
 * The copy used here makes rsmi7 the second partition of its GPU, which
 * is a GPU nothing can name.
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
#include <unistd.h>

#define TESTNS "pgpu-accel"

static const struct {
    const char *devs[2];   /* the GPUs the rank was mapped against */
    const char *want;      /* ROCR_VISIBLE_DEVICES, or NULL for unset */
} ranks[] = {
    {{"rsmi0", NULL}, "GPU-0000000000000001"},
    {{"rsmi1", NULL}, "GPU-0000000000000002"},
    {{"rsmi2", "rsmi3"}, "GPU-0000000000000003,GPU-0000000000000004"},
    /* the partition: nothing to name it by */
    {{"rsmi7", NULL}, NULL},
    /* one nameable GPU and one not: naming the first alone would hide
     * the second, where saying nothing leaves both visible */
    {{"rsmi6", "rsmi7"}, NULL},
};
#define NRANKS (sizeof(ranks) / sizeof(ranks[0]))

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

/* Copy the fixture to a temporary file with rsmi7 made the second
 * partition of its GPU.  The caller unlinks it. */
static int write_fixture(const char *src, char *dst, size_t dlen)
{
    static const char from[] = "000075a0-0000-1000-8000-000000000008";
    static const char to[] = "000175a0-0000-1000-8000-000000000008";
    const char *tmp = getenv("TMPDIR");
    char *buf, *hit;
    long sz;
    FILE *fp;
    int fd;

    fp = fopen(src, "r");
    if (NULL == fp) {
        return -1;
    }
    if (0 != fseek(fp, 0, SEEK_END) || 0 > (sz = ftell(fp)) || 0 != fseek(fp, 0, SEEK_SET)) {
        fclose(fp);
        return -1;
    }
    buf = malloc((size_t) sz + 1);
    if (NULL == buf || (size_t) sz != fread(buf, 1, (size_t) sz, fp)) {
        fclose(fp);
        free(buf);
        return -1;
    }
    fclose(fp);
    buf[sz] = '\0';
    hit = strstr(buf, from);
    if (NULL == hit || NULL != strstr(hit + 1, from)) {
        free(buf);
        return -1;
    }
    memcpy(hit, to, sizeof(to) - 1);

    snprintf(dst, dlen, "%s/pgpu-accel-XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    fd = mkstemp(dst);
    if (0 > fd) {
        free(buf);
        return -1;
    }
    if ((ssize_t) sz != write(fd, buf, (size_t) sz)) {
        close(fd);
        unlink(dst);
        free(buf);
        return -1;
    }
    close(fd);
    free(buf);
    return 0;
}

/* Register a namespace whose rank m was mapped against ranks[m].devs. */
static pmix_status_t register_nspace(void)
{
    pmix_info_t *info, *pdata;
    pmix_data_array_t *array, *darray;
    pmix_device_t *dev;
    pmix_status_t rc;
    pmix_proc_t p;
    size_t ninfo, n = 0, k, ndevs;
    uint32_t nprocs = (uint32_t) NRANKS;
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

        ndevs = (NULL == ranks[m].devs[1]) ? 1 : 2;
        PMIX_DATA_ARRAY_CREATE(darray, ndevs, PMIX_DEVICE);
        dev = (pmix_device_t *) darray->array;
        for (k = 0; k < ndevs; k++) {
            PMIx_Device_construct(&dev[k]);
            dev[k].osname = strdup(ranks[m].devs[k]);
            pmix_asprintf(&dev[k].uuid, "gpu://%s::%s", pmix_globals.hostname,
                          ranks[m].devs[k]);
            dev[k].type = PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC;
        }
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
    char src[1024], path[1024];
    size_t m;

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    snprintf(src, sizeof(src), "%s/mi350-bcm57608.xml", PMIX_TEST_TOPO_DIR);
#endif
    if (0 != write_fixture(src, path, sizeof(path))) {
        fprintf(stderr, "FAIL: could not write the edited fixture\n");
        return 1;
    }

    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        unlink(path);
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
    for (m = 0; m < NRANKS; m++) {
        PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) m);
        env = NULL;
        rc = pmix_pgpu_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds");
        val = envval(env, "ROCR_VISIBLE_DEVICES");
        if (NULL == ranks[m].want) {
            ok(NULL == val, "a rank with a GPU that cannot be named is told nothing");
        } else {
            ok(NULL != val && 0 == strcmp(val, ranks[m].want),
               "the rank is given its own GPUs, in ROCm's form");
        }
        if ((NULL == ranks[m].want) != (NULL == val)
            || (NULL != val && 0 != strcmp(val, ranks[m].want))) {
            fprintf(stderr, "      rank %d: got %s, wanted %s\n", (int) m,
                    (NULL == val) ? "nothing" : val,
                    (NULL == ranks[m].want) ? "nothing" : ranks[m].want);
        }
        /* and no other vendor's variable claims them */
        ok(NULL == envval(env, "CUDA_VISIBLE_DEVICES"), "no NVIDIA variable is written");
        ok(NULL == envval(env, "ZE_AFFINITY_MASK"), "no Intel variable is written");
        PMIx_Argv_free(env);
    }

done:
    PMIx_server_finalize();
    unlink(path);
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
