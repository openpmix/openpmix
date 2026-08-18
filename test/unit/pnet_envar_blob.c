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
 * Unit tests for the envar blob a pnet component builds on the lead server
 * and reads back on a compute node: allocate -> setup_local_network ->
 * setup_fork.
 *
 * The two halves are written in different files at different times and
 * nothing but this round trip checks that they still agree.  Two ways they
 * stopped agreeing, both of which this covers:
 *
 * - The unpack loop can only end by running out of envars, so the status it
 *   leaves behind is PMIX_ERR_UNPACK_READ_PAST_END_OF_BUFFER.  Returning
 *   that says "local setup failed" to the base, which stops the fan-out and
 *   fails PMIx_server_setup_local_support for the whole job - and it says
 *   so on the very path that worked, so a job that harvested nothing was
 *   the only kind that launched.
 * - A blob with no envars in it is the ordinary case for a job that asked
 *   for none, and it must be as quiet as a full one.
 *
 * It runs against test-topo2.xml, handed to the server through the
 * documented topo_file hook, because that fixture carries the Mellanox
 * controller the nvd component opens for - which is what puts a component
 * with an envar blob into the active list on a machine with no such card.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/include/pmix_globals.h"
#include "src/class/pmix_list.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/nvd/pnet_nvd.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TESTNS "pnet-envar-blob"

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
    rc = pmix_pnet.allocate(nspace, &directive, 1, &ilist);
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
    snprintf(path, sizeof(path), "%s/test-topo2.xml", PMIX_TEST_TOPO_DIR);
#endif

    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    /* what the component is being asked to carry to the compute nodes.
     * These match its default include globs, and they are set here rather
     * than assumed of the environment the test was run in */
    setenv("UCX_TLS", "rc_x,sm", 1);
    setenv("NCCL_DEBUG", "INFO", 1);
    /* an envar with an "=" in its value: the pack/unpack pair carries name
     * and value separately, so a value that looks like an assignment must
     * survive intact */
    setenv("UCC_TL_UCP_TUNE", "inter:0-4K:@0", 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 77;
    }

    /* --- a blob with envars in it --- */

    rc = make_blob(TESTNS, true, &blob, &nblob);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: no pnet component contributed a blob: %s\n",
                PMIx_Error_string(rc));
        PMIx_server_finalize();
        return 77;
    }
    ok(has_key(blob, nblob, PMIX_PNET_NVD_BLOB),
       "the component that opened for this fabric contributed its blob");

    /* the compute-node half.  Success here is the whole point: any other
     * status aborts the fan-out and fails setup_local_support for the job */
    rc = pmix_pnet.setup_local_network(TESTNS, blob, nblob);
    ok(PMIX_SUCCESS == rc, "a blob carrying envars sets up cleanly");
    PMIX_INFO_FREE(blob, nblob);
    blob = NULL;

    /* and the envars must have made the trip */
    PMIX_LOAD_PROCID(&proc, TESTNS, 0);
    env = NULL;
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds");
    ev = envval(env, "UCX_TLS");
    ok(NULL != ev && 0 == strcmp(ev, "rc_x,sm"),
       "a harvested envar reaches the child");
    ev = envval(env, "NCCL_DEBUG");
    ok(NULL != ev && 0 == strcmp(ev, "INFO"),
       "and so does one from another glob in the list");
    ev = envval(env, "UCC_TL_UCP_TUNE");
    ok(NULL != ev && 0 == strcmp(ev, "inter:0-4K:@0"),
       "a value containing '=' is not truncated");
    PMIx_Argv_free(env);
    env = NULL;

    /* --- a blob with nothing in it --- */

    rc = make_blob(TESTNS "-empty", false, &blob, &nblob);
    ok(PMIX_SUCCESS == rc, "a job that asked for no envars still allocates");
    if (PMIX_SUCCESS == rc) {
        rc = pmix_pnet.setup_local_network(TESTNS "-empty", blob, nblob);
        ok(PMIX_SUCCESS == rc, "an empty blob is not an error either");
        PMIX_INFO_FREE(blob, nblob);
        blob = NULL;

        PMIX_LOAD_PROCID(&proc, TESTNS "-empty", 0);
        env = NULL;
        rc = pmix_pnet_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc && NULL == envval(env, "UCX_TLS"),
           "and contributes nothing to the child");
        PMIx_Argv_free(env);
        env = NULL;
    }

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
    PMIX_LOAD_KEY(blob[0].key, PMIX_PNET_NVD_BLOB);
    blob[0].value.type = PMIX_COMPRESSED_BYTE_OBJECT;
    blob[0].value.data.bo.bytes = (char *) malloc(sizeof(junk));
    memcpy(blob[0].value.data.bo.bytes, junk, sizeof(junk));
    blob[0].value.data.bo.size = sizeof(junk);
    rc = pmix_pnet.setup_local_network(TESTNS "-junk", blob, nblob);
    ok(PMIX_SUCCESS != rc, "a blob that cannot be decompressed is refused");
    PMIX_INFO_FREE(blob, nblob);

    PMIx_server_finalize();
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
