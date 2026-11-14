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
 * Unit tests for the vendor "visible devices" environment a pgpu module
 * contributes to a process it is about to fork.
 *
 * The thing under test is the whole chain, because every link of it was
 * broken or absent until now: PMIx_server_setup_fork never called
 * pmix_pgpu.setup_fork at all, so the framework's environment handling was
 * unreachable; the module interface had no per-process hook, so a value
 * that differs per rank had nowhere to be set; and the vendor's own device
 * identifier - the only string a GPU runtime will accept - was not read out
 * of the topology by anything.
 *
 * The test runs against nvml-4gpu.xml, handed to the server through the
 * documented topo_file hook. That fixture is a real machine read by an
 * hwloc with the CUDA and NVML backends, and it matters here for two
 * reasons beyond carrying UUIDs at all: its PCI functions are NVIDIA
 * display-class devices, so the nvd component's own open-time vendor check
 * passes and the module really is selected on a machine that has no GPU;
 * and each function exposes cuda0/opencl0d0/nvml0 with the identity on
 * nvml0, so a value only appears if the identity was found across the
 * function rather than on the device that names it.
 *
 * The copy used here has the identity of the fourth GPU, cuda3, removed,
 * which is how a node presents one whose GPUs NVML reported only some of.
 * A GPU with no identity is an ordinary shape - every NVIDIA GPU on a node
 * whose hwloc has no NVML backend is one - and it is still NVIDIA's: a
 * process given it alongside a GPU that can be named must be told nothing,
 * since naming the other alone would hide it.
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
#include <unistd.h>

#define TESTNS "pgpu-visdev"

/* the first two GPUs of the fixture, and their NVIDIAUUIDs */
#define NPROCS 2
static const char *osnames[NPROCS] = {"cuda0", "cuda1"};
static const char *uuids[NPROCS] = {
    "GPU-46f77619-68bf-8f9d-7cfb-61fa9c4ca692",
    "GPU-32559d2f-bd80-6360-09ab-97af8614d541"
};

/* Ranks NPROCS + 1 on, each mapped against GPUs one of which has no
 * identity.  Rank NPROCS itself is registered but not mapped. */
static const struct {
    const char *devs[2];
    const char *why;
} unnamed[] = {
    {{"cuda3", NULL}, "a rank given a GPU with no identity is told nothing"},
    {{"cuda2", "cuda3"}, "a rank given a nameable GPU and one with no identity is told nothing"},
    {{"cuda3", "cuda2"}, "the same, whichever order the GPUs were assigned in"},
};
#define NUNNAMED (sizeof(unnamed) / sizeof(unnamed[0]))
#define NRANKS (NPROCS + 1 + NUNNAMED)

/* what the copy takes away: cuda3's identity, which hwloc's NVML backend
 * recorded on nvml3 */
static const char strip[] =
    "<info name=\"NVIDIAUUID\" value=\"GPU-81addb77-2337-6f9f-b0dd-696aa82969d1\"/>";

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

/* Copy the fixture to a temporary file without cuda3's identity.  The
 * caller unlinks it. */
static int write_fixture(const char *src, char *dst, size_t dlen)
{
    const char *tmp = getenv("TMPDIR");
    char *buf, *hit;
    size_t len, cut = sizeof(strip) - 1;
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
    hit = strstr(buf, strip);
    if (NULL == hit || NULL != strstr(hit + 1, strip)) {
        free(buf);
        return -1;
    }
    memmove(hit, hit + cut, strlen(hit + cut) + 1);
    len = strlen(buf);

    snprintf(dst, dlen, "%s/pgpu-visdev-XXXXXX", (NULL == tmp) ? "/tmp" : tmp);
    fd = mkstemp(dst);
    if (0 > fd) {
        free(buf);
        return -1;
    }
    if ((ssize_t) len != write(fd, buf, len)) {
        close(fd);
        unlink(dst);
        free(buf);
        return -1;
    }
    close(fd);
    free(buf);
    return 0;
}

/* the GPUs rank m was mapped against: one each for the first NPROCS, none
 * for rank NPROCS - which is what every ordinary job looks like - and the
 * pairs in unnamed[] after it */
static size_t rank_devs(uint32_t m, const char **devs)
{
    size_t n = 0;

    if (m < NPROCS) {
        devs[n++] = osnames[m];
    } else if (m > NPROCS) {
        devs[n++] = unnamed[m - NPROCS - 1].devs[0];
        if (NULL != unnamed[m - NPROCS - 1].devs[1]) {
            devs[n++] = unnamed[m - NPROCS - 1].devs[1];
        }
    }
    return n;
}

static pmix_status_t register_nspace(void)
{
    pmix_info_t *info, *pdata;
    pmix_data_array_t *array, *darray;
    pmix_device_t *dev;
    pmix_status_t rc;
    pmix_proc_t p;
    size_t ninfo, n = 0, d, ndevs;
    uint32_t nprocs = NRANKS;
    uint32_t m;
    uint16_t lrank;
    pmix_nspace_t ns;
    const char *devs[2];

    ninfo = 2 + nprocs;
    PMIX_INFO_CREATE(info, ninfo);

    PMIX_INFO_LOAD(&info[n], PMIX_JOB_SIZE, &nprocs, PMIX_UINT32);
    ++n;
    PMIX_INFO_LOAD(&info[n], PMIX_LOCAL_SIZE, &nprocs, PMIX_UINT32);
    ++n;

    for (m = 0; m < nprocs; m++) {
        size_t k = 0;

        ndevs = rank_devs(m, devs);
        pmix_strncpy(info[n].key, PMIX_PROC_INFO_ARRAY, PMIX_MAX_KEYLEN);
        info[n].value.type = PMIX_DATA_ARRAY;
        PMIX_DATA_ARRAY_CREATE(array, (0 < ndevs) ? 3 : 2, PMIX_INFO);
        info[n].value.data.darray = array;
        pdata = (pmix_info_t *) array->array;

        PMIX_LOAD_PROCID(&p, TESTNS, (pmix_rank_t) m);
        PMIX_INFO_LOAD(&pdata[k], PMIX_PROCID, &p, PMIX_PROC);
        ++k;
        lrank = (uint16_t) m;
        PMIX_INFO_LOAD(&pdata[k], PMIX_LOCAL_RANK, &lrank, PMIX_UINT16);
        ++k;

        if (0 < ndevs) {
            /* always an array of pmix_device_t, even for one device */
            PMIX_DATA_ARRAY_CREATE(darray, ndevs, PMIX_DEVICE);
            dev = (pmix_device_t *) darray->array;
            for (d = 0; d < ndevs; d++) {
                PMIx_Device_construct(&dev[d]);
                dev[d].osname = strdup(devs[d]);
                pmix_asprintf(&dev[d].uuid, "gpu://%s::%s",
                              pmix_globals.hostname, devs[d]);
                dev[d].type = PMIX_DEVTYPE_GPU | PMIX_DEVTYPE_COPROC;
            }
            /* hand the array over rather than PMIX_INFO_LOAD-ing it: the
             * info now owns it, which is how the shipped proc-array test
             * builds one too */
            pmix_strncpy(pdata[k].key, PMIX_DEVICE_ID, PMIX_MAX_KEYLEN);
            pdata[k].value.type = PMIX_DATA_ARRAY;
            pdata[k].value.data.darray = darray;
        }
        ++n;
    }

    /* No callback, which is what makes this call BLOCK - PMIx substitutes
     * an internal lock when it is given none.  With a callback the call
     * returns immediately and the info array below would be freed while
     * the server was still reading it, which shows up as a value of some
     * impossible type rather than as a use-after-free. */
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
    snprintf(src, sizeof(src), "%s/nvml-4gpu.xml", PMIX_TEST_TOPO_DIR);
#endif
    if (0 != write_fixture(src, path, sizeof(path))) {
        fprintf(stderr, "FAIL: could not write the edited fixture\n");
        return 1;
    }

    /* Hand the server the fixture as its own topology.  This is the
     * documented testing hook, and it is what lets the nvd component's
     * open-time NVIDIA check pass on a machine with no GPU in it. */
    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        unlink(path);
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

    /* each mapped rank is told about its own GPU, by the vendor's name for
     * it - which lives on a sibling of the device the assignment names */
    for (m = 0; m < NPROCS; m++) {
        PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) m);
        env = NULL;
        rc = pmix_pgpu_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds");
        val = envval(env, "CUDA_VISIBLE_DEVICES");
        ok(NULL != val && 0 == strcmp(val, uuids[m]),
           "the rank is given its own GPU's vendor identity");
        if (NULL != val && 0 != strcmp(val, uuids[m])) {
            fprintf(stderr, "      rank %d: got %s, wanted %s\n",
                    (int) m, val, uuids[m]);
        }
        /* never the vendor's ordering variable: the identity does not
         * depend on it, and overriding it would renumber every device for
         * the rest of the process's life */
        ok(NULL == envval(env, "CUDA_DEVICE_ORDER"),
           "the device ordering is left alone");
        PMIx_Argv_free(env);
    }

    /* two ranks on one node never get the same GPU named */
    ok(0 != strcmp(uuids[0], uuids[1]), "the fixture's GPUs are distinguishable");

    /* a rank that was not mapped against a device gets nothing.  This is
     * the ordinary case for every job that does not use --map-by device,
     * so it has to be silent rather than an error */
    PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) NPROCS);
    env = NULL;
    rc = pmix_pgpu_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "an unmapped rank is not an error");
    ok(NULL == envval(env, "CUDA_VISIBLE_DEVICES"),
       "an unmapped rank is told nothing");
    PMIx_Argv_free(env);

    /* a GPU with no identity is still NVIDIA's, and one of the process's:
     * an unset variable leaves it visible, where a list without it would
     * hide it */
    for (m = 0; m < NUNNAMED; m++) {
        PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) (NPROCS + 1 + m));
        env = NULL;
        rc = pmix_pgpu_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds");
        val = envval(env, "CUDA_VISIBLE_DEVICES");
        ok(NULL == val, unnamed[m].why);
        if (NULL != val) {
            fprintf(stderr, "      rank %d: got %s, wanted nothing\n",
                    (int) (NPROCS + 1 + m), val);
        }
        PMIx_Argv_free(env);
    }

    /* the vendor filter: asking for AMD's devices on a machine that has
     * only NVIDIA ones must decline rather than name them.  A node can
     * carry cards from two vendors and each wants its own variable */
    PMIX_LOAD_PROCID(&proc, TESTNS, 0);
    env = NULL;
    rc = pmix_pgpu_base_set_visible_devices(&proc, "AMD",
                                            "ROCR_VISIBLE_DEVICES", &env);
    ok(PMIX_ERR_TAKE_NEXT_OPTION == rc, "another vendor's devices are declined");
    ok(NULL == envval(env, "ROCR_VISIBLE_DEVICES"),
       "and no value is written for them");
    PMIx_Argv_free(env);

    /* and an NVIDIA GPU with no identity is not mistaken for another
     * vendor's because it carries no name to say whose it is */
    PMIX_LOAD_PROCID(&proc, TESTNS, (pmix_rank_t) (NPROCS + 1));
    env = NULL;
    rc = pmix_pgpu_base_set_visible_devices(&proc, "AMD",
                                            "ROCR_VISIBLE_DEVICES", &env);
    ok(PMIX_ERR_TAKE_NEXT_OPTION == rc && NULL == envval(env, "ROCR_VISIBLE_DEVICES"),
       "an NVIDIA GPU with no identity is not named as another vendor's");
    PMIx_Argv_free(env);

done:
    PMIx_server_finalize();
    unlink(path);
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
