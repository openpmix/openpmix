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
 * Unit tests for the envar blob a pgpu component builds on the lead server
 * and reads back on a compute node: allocate -> setup_local -> setup_fork.
 *
 * The two halves are written in different files at different times and
 * nothing but this round trip checks that they still agree.  Three ways
 * they stopped agreeing, all of which this covers:
 *
 * - The unpack loop can only end by running out of envars, so the status it
 *   leaves behind is PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER.  Returning
 *   that says "local setup failed" to the base, which stops the fan-out and
 *   fails PMIx_server_setup_local_support for the whole job.
 * - A job that asked for no envars has nothing to ship, and the components
 *   decline rather than appending an empty blob for every daemon to carry.
 * - The compressed half of the blob is written by whichever node ran
 *   allocate and read by whichever node runs setup_local, and those need
 *   not have been built with the same compression support.
 *
 * It runs against nvml-4gpu.xml, handed to the server through the
 * documented topo_file hook, because that fixture carries the NVIDIA
 * display-class functions the nvd component opens for - which is what puts
 * a component with an envar blob into the active list on a machine with no
 * GPU.  nvd is also the one pgpu component with a non-empty default include
 * list, so it has something to harvest without being reconfigured.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/mca/pgpu/base/base.h"
#include "src/mca/pgpu/nvd/pgpu_nvd.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TESTNS "pgpu-envar-blob"

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

/* Run allocate for one namespace and hand back the blob our component
 * produced, as the pmix_info_t array a compute-node daemon would see.
 * The caller frees it with PMIX_INFO_FREE. */
static pmix_status_t make_blob(char *nspace, bool envars,
                               pmix_info_t **info, size_t *ninfo)
{
    pmix_list_t ilist;
    pmix_info_t directive;
    pmix_kval_t *kv;
    pmix_info_t *blobs;
    size_t n, nblobs;
    pmix_status_t rc;

    *info = NULL;
    *ninfo = 0;

    PMIX_CONSTRUCT(&ilist, pmix_list_t);
    PMIX_INFO_LOAD(&directive, PMIX_SETUP_APP_ENVARS, &envars, PMIX_BOOL);
    rc = pmix_pgpu.allocate(nspace, &directive, 1, &ilist);
    PMIX_INFO_DESTRUCT(&directive);
    if (PMIX_SUCCESS != rc) {
        PMIX_LIST_DESTRUCT(&ilist);
        return rc;
    }

    nblobs = pmix_list_get_size(&ilist);
    if (0 == nblobs) {
        PMIX_LIST_DESTRUCT(&ilist);
        return PMIX_ERR_NOT_FOUND;
    }

    /* the list carries one kval per active component; hand them all on,
     * which is also what the host does */
    PMIX_INFO_CREATE(blobs, nblobs);
    n = 0;
    PMIX_LIST_FOREACH (kv, &ilist, pmix_kval_t) {
        PMIX_LOAD_KEY(blobs[n].key, kv->key);
        PMIx_Value_xfer(&blobs[n].value, kv->value);
        ++n;
    }
    PMIX_LIST_DESTRUCT(&ilist);

    *info = blobs;
    *ninfo = nblobs;
    return PMIX_SUCCESS;
}

static bool has_key(pmix_info_t *info, size_t ninfo, const char *key)
{
    size_t n;

    for (n = 0; n < ninfo; n++) {
        if (PMIX_CHECK_KEY(&info[n], key)) {
            return true;
        }
    }
    return false;
}

int main(int argc, char **argv)
{
    pmix_info_t *blob = NULL;
    size_t nblob = 0;
    char **env = NULL;
    const char *ev;
    pmix_proc_t proc;
    pmix_status_t rc;
    char path[1024];
    uint8_t junk[16];
    uint32_t junklen;

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    snprintf(path, sizeof(path), "%s/nvml-4gpu.xml", PMIX_TEST_TOPO_DIR);
#endif

    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    /* what the component is being asked to carry to the compute nodes.
     * These match its default include globs, and they are set here rather
     * than assumed of the environment the test was run in */
    setenv("CUDA_CACHE_PATH", "/tmp/cuda-cache", 1);
    setenv("NCCL_DEBUG", "INFO", 1);
    /* an envar with an "=" in its value: the pack/unpack pair carries name
     * and value separately, so a value that looks like an assignment must
     * survive intact */
    setenv("NCCL_TUNER_CONFIG", "coll=allreduce:0-4K", 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 77;
    }

    /* --- a blob with envars in it --- */

    rc = make_blob(TESTNS, true, &blob, &nblob);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: no pgpu component contributed a blob: %s\n",
                PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 77;
    }
    ok(has_key(blob, nblob, PMIX_PGPU_NVD_BLOB),
       "the component that opened for this topology contributed its blob");

    /* the compute-node half.  Success here is the whole point: any other
     * status aborts the fan-out and fails setup_local_support for the job */
    rc = pmix_pgpu.setup_local(TESTNS, blob, nblob);
    ok(PMIX_SUCCESS == rc, "a blob carrying envars sets up cleanly");
    PMIX_INFO_FREE(blob, nblob);
    blob = NULL;

    /* and the envars must have made the trip */
    PMIX_LOAD_PROCID(&proc, TESTNS, 0);
    env = NULL;
    rc = pmix_pgpu.setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds");
    ev = envval(env, "CUDA_CACHE_PATH");
    ok(NULL != ev && 0 == strcmp(ev, "/tmp/cuda-cache"),
       "a harvested envar reaches the child");
    ev = envval(env, "NCCL_DEBUG");
    ok(NULL != ev && 0 == strcmp(ev, "INFO"),
       "and so does one from another glob in the list");
    ev = envval(env, "NCCL_TUNER_CONFIG");
    ok(NULL != ev && 0 == strcmp(ev, "coll=allreduce:0-4K"),
       "a value containing '=' is not truncated");
    PMIx_Argv_free(env);
    env = NULL;

    /* the cache and the namespace reference it holds are released here and
     * nowhere else, so a server that never deregisters accumulates one of
     * each per job */
    pmix_pgpu.deregister_nspace(TESTNS);
    PMIX_LOAD_PROCID(&proc, TESTNS, 0);
    env = NULL;
    rc = pmix_pgpu.setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc && NULL == envval(env, "CUDA_CACHE_PATH"),
       "deregistering the nspace drops its cached envars");
    PMIx_Argv_free(env);
    env = NULL;

    /* --- a job that asked for no envars --- */

    /* nothing to harvest means nothing to say: appending an empty blob
     * would ship one to every daemon in the job to say so */
    rc = make_blob(TESTNS "-empty", false, &blob, &nblob);
    ok(PMIX_ERR_NOT_FOUND == rc, "a job that asked for no envars gets no blob");
    if (PMIX_SUCCESS == rc) {
        PMIX_INFO_FREE(blob, nblob);
        blob = NULL;
    }

    /* setup_local with nothing of ours in the array is still success - the
     * base fans out to every component and one that finds no blob of its
     * own has not failed at anything */
    rc = pmix_pgpu.setup_local(TESTNS "-empty", NULL, 0);
    ok(PMIX_SUCCESS == rc, "an info array carrying no blob is not an error");
    pmix_pgpu.deregister_nspace(TESTNS "-empty");

    /* --- a blob nobody can read --- */

    /* A node that built no compression component still receives compressed
     * blobs from one that did, and the decompressor then writes nothing
     * back.  Reading, or freeing, whatever it did not write is the failure
     * this guards; declining the blob is the answer. */
    junklen = 8;
    memcpy(junk, &junklen, sizeof(junklen)); // the uncompressed size goes first
    memset(&junk[sizeof(junklen)], 0xa5, sizeof(junk) - sizeof(junklen));
    PMIX_INFO_CREATE(blob, 1);
    nblob = 1;
    PMIX_LOAD_KEY(blob[0].key, PMIX_PGPU_NVD_BLOB);
    blob[0].value.type = PMIX_COMPRESSED_BYTE_OBJECT;
    blob[0].value.data.bo.bytes = (char *) malloc(sizeof(junk));
    memcpy(blob[0].value.data.bo.bytes, junk, sizeof(junk));
    blob[0].value.data.bo.size = sizeof(junk);
    rc = pmix_pgpu.setup_local(TESTNS "-junk", blob, nblob);
    ok(PMIX_SUCCESS != rc, "a blob that cannot be decompressed is refused");
    PMIX_INFO_FREE(blob, nblob);
    pmix_pgpu.deregister_nspace(TESTNS "-junk");

    PMIx_server_finalize();
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
