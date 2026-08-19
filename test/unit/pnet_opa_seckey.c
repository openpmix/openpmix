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
 * Unit tests for the one thing the pnet opa component does that no other
 * shipped component does: the transport "pre-conditioning" key.
 *
 * A job's processes have to agree on that key before any of them can
 * generate an endpoint address, so it is generated once on the lead server
 * inside allocate, carried to the compute nodes in opa's blob, and put
 * back into every child's environment as
 * OMPI_MCA_orte_precondition_transports.  Nothing else in the tree reads
 * that name, and a job that launches without the key does not run slower -
 * it runs unconditioned.
 *
 * It also covers the request that asks for the key, which arrives as the
 * deprecated PMIX_ALLOC_NETWORK data array.  Deprecated is exactly why its
 * shape has to be checked rather than assumed: opa is the only reader left
 * in the library, so a host that gets the type wrong is corrected by
 * nobody, and reading a string's bytes as a pmix_data_array_t walks a wild
 * pointer for a garbage count.  Those cases assert survival, not a return
 * code - a directive the component cannot use is legitimately ignored.
 *
 * It runs against opa-hfi.xml, handed to the server through the documented
 * topo_file hook.  That fixture carries an Intel Omni-Path HFI (PCI
 * 8086 class 0208), which is what puts opa into the active list on a
 * machine with no such card, and an Intel *ethernet* controller (class
 * 0200) beside it, because "same vendor, wrong class" is the mistake the
 * component's PCI filter exists to prevent.
 */

#include "src/include/pmix_config.h"
#include "include/pmix.h"
#include "include/pmix_server.h"
#include "src/class/pmix_list.h"
#include "src/include/pmix_globals.h"
#include "src/mca/gds/gds.h"
#include "src/mca/pnet/base/base.h"
#include "src/mca/pnet/opa/pnet_opa.h"
#include "src/util/pmix_argv.h"
#include "src/util/pmix_environ.h"
#include "src/util/pmix_printf.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NPROCS 2
#define TESTNS "pnet-opa"
#define XPORT  "OMPI_MCA_orte_precondition_transports"

/* rank 0 gets the HFI, rank 1 the ethernet controller from the same
 * vendor - which opa must not claim */
static const char *osnames[NPROCS] = {"hfi1_0", "enp66s0"};

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

/* the key is two 64-bit numbers in hex with a dash between them, and its
 * consumers parse it that way - so its shape is part of the contract */
static bool is_transport_key(const char *val)
{
    const char *dash;
    size_t n;

    if (NULL == val) {
        return false;
    }
    dash = strchr(val, '-');
    if (NULL == dash || dash == val || '\0' == dash[1]) {
        return false;
    }
    for (n = 0; '\0' != val[n]; n++) {
        if ('-' != val[n] && !isxdigit((unsigned char) val[n])) {
            return false;
        }
    }
    return true;
}

/* Run allocate for one namespace and hand back whatever blobs the active
 * components produced, as the pmix_info_t array a compute-node daemon
 * would see.  The caller frees it with PMIX_INFO_FREE. */
static pmix_status_t make_blob(char *nspace, pmix_info_t *directives, size_t ndirs,
                               pmix_info_t **info, size_t *ninfo)
{
    pmix_list_t ilist;
    pmix_kval_t *kv;
    pmix_info_t *blobs;
    size_t n, nblobs;
    pmix_status_t rc;

    *info = NULL;
    *ninfo = 0;

    PMIX_CONSTRUCT(&ilist, pmix_list_t);
    rc = pmix_pnet.allocate(nspace, directives, ndirs, &ilist);
    if (PMIX_SUCCESS != rc) {
        PMIX_LIST_DESTRUCT(&ilist);
        return rc;
    }

    nblobs = pmix_list_get_size(&ilist);
    if (0 == nblobs) {
        PMIX_LIST_DESTRUCT(&ilist);
        return PMIX_ERR_NOT_FOUND;
    }

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

/* Ask allocate for a seckey, deliver the blob, and report the key the
 * child would be forked with.  The caller frees the returned string. */
static char *seckey_round_trip(char *nspace, pmix_info_t *directives, size_t ndirs)
{
    pmix_info_t *blob = NULL;
    size_t nblob = 0;
    char **env = NULL;
    const char *ev;
    pmix_proc_t proc;
    pmix_status_t rc;
    char *answer = NULL;

    if (PMIX_SUCCESS != make_blob(nspace, directives, ndirs, &blob, &nblob)) {
        return NULL;
    }
    rc = pmix_pnet.setup_local_network(nspace, blob, nblob);
    PMIX_INFO_FREE(blob, nblob);
    if (PMIX_SUCCESS != rc) {
        return NULL;
    }

    PMIX_LOAD_PROCID(&proc, nspace, 0);
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    if (PMIX_SUCCESS == rc) {
        ev = envval(env, XPORT);
        if (NULL != ev) {
            answer = strdup(ev);
        }
    }
    PMIx_Argv_free(env);
    return answer;
}

/* Register a namespace whose ranks were mapped against devices, the way
 * pnet_assigned_devices.c does - rank 0 the HFI, rank 1 the ethernet
 * controller from the same vendor. */
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
        size_t k = 0;

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

        PMIX_DATA_ARRAY_CREATE(darray, 1, PMIX_DEVICE);
        dev = (pmix_device_t *) darray->array;
        PMIx_Device_construct(&dev[0]);
        dev[0].osname = strdup(osnames[m]);
        pmix_asprintf(&dev[0].uuid, "nic://%s::%s", pmix_globals.hostname, osnames[m]);
        dev[0].type = (0 == m) ? PMIX_DEVTYPE_OPENFABRICS : PMIX_DEVTYPE_NETWORK;
        /* hand the array over rather than PMIX_INFO_LOAD-ing it: the info
         * now owns it */
        pmix_strncpy(pdata[k].key, PMIX_DEVICE_ID, PMIX_MAX_KEYLEN);
        pdata[k].value.type = PMIX_DATA_ARRAY;
        pdata[k].value.data.darray = darray;
        ++n;
    }

    /* No callback, which is what makes this call BLOCK */
    PMIX_LOAD_NSPACE(ns, TESTNS);
    rc = PMIx_server_register_nspace(ns, nprocs, info, ninfo, NULL, NULL);
    PMIX_INFO_FREE(info, ninfo);
    return rc;
}

int main(int argc, char **argv)
{
    pmix_info_t *blob = NULL;
    pmix_info_t directive, netdir;
    pmix_info_t *netarray;
    pmix_data_array_t *darray;
    size_t nblob = 0;
    char **env = NULL;
    const char *ev;
    char *key1 = NULL, *key2 = NULL;
    pmix_proc_t proc;
    pmix_status_t rc;
    bool flag = true;
    char path[1024];

    (void) argc;
    (void) argv;

#ifndef PMIX_TEST_TOPO_DIR
    fprintf(stderr, "SKIP: no topology directory configured\n");
    return 77;
#else
    snprintf(path, sizeof(path), "%s/opa-hfi.xml", PMIX_TEST_TOPO_DIR);
#endif

    /* Hand the server the fixture as its own topology.  This is the
     * documented testing hook, and it is what lets opa's open-time
     * Omni-Path check pass on a machine with no HFI in it. */
    setenv("PMIX_MCA_pmix_hwloc_topo_file", path, 1);

    rc = PMIx_server_init(NULL, NULL, 0);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: PMIx_server_init failed: %s\n", PMIx_Error_string(rc));
        return 77;
    }

    /* --- the key, asked for the plain way --- */

    PMIX_INFO_LOAD(&directive, PMIX_SETUP_APP_ALL, &flag, PMIX_BOOL);
    rc = make_blob(TESTNS, &directive, 1, &blob, &nblob);
    if (PMIX_SUCCESS != rc) {
        fprintf(stderr, "SKIP: opa contributed no blob: %s\n", PMIx_Error_string(rc));
        PMIX_INFO_DESTRUCT(&directive);
        PMIx_server_finalize();
        return 77;
    }
    ok(PMIX_CHECK_KEY(&blob[0], PMIX_PNET_OPA_BLOB),
       "the component that opened for this fabric contributed its blob");
    rc = pmix_pnet.setup_local_network(TESTNS, blob, nblob);
    ok(PMIX_SUCCESS == rc, "the blob sets up cleanly on the compute node");
    PMIX_INFO_FREE(blob, nblob);
    blob = NULL;

    PMIX_LOAD_PROCID(&proc, TESTNS, 0);
    env = NULL;
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    ok(PMIX_SUCCESS == rc, "setup_fork succeeds");
    ev = envval(env, XPORT);
    ok(is_transport_key(ev), "the child is forked with a well-formed transport key");
    PMIx_Argv_free(env);
    env = NULL;
    PMIX_INFO_DESTRUCT(&directive);

    /* --- the key, asked for through the deprecated network array --- */

    /* PMIX_ALLOC_NETWORK_SEC_KEY nested in a PMIX_ALLOC_NETWORK array is
     * the other way to ask, and the only way that reaches the nested scan */
    PMIX_DATA_ARRAY_CREATE(darray, 1, PMIX_INFO);
    netarray = (pmix_info_t *) darray->array;
    PMIX_INFO_LOAD(&netarray[0], PMIX_ALLOC_NETWORK_SEC_KEY, &flag, PMIX_BOOL);
    PMIX_LOAD_KEY(netdir.key, PMIX_ALLOC_NETWORK);
    netdir.flags = 0;
    netdir.value.type = PMIX_DATA_ARRAY;
    netdir.value.data.darray = darray;

    key1 = seckey_round_trip(TESTNS "-net", &netdir, 1);
    ok(is_transport_key(key1), "a nested seckey request is honored");
    PMIX_INFO_DESTRUCT(&netdir);

    /* --- and every job gets its own --- */

    PMIX_INFO_LOAD(&directive, PMIX_SETUP_APP_ALL, &flag, PMIX_BOOL);
    key2 = seckey_round_trip(TESTNS "-two", &directive, 1);
    ok(is_transport_key(key2), "a second job gets a key too");
    ok(NULL != key1 && NULL != key2 && 0 != strcmp(key1, key2),
       "and it is not the first job's key");
    free(key1);
    free(key2);
    PMIX_INFO_DESTRUCT(&directive);

    /* --- a network directive the component cannot read --- */

    /* The attribute is documented as a data array.  A host that hands over
     * a string instead used to have its bytes read as one - a wild pointer
     * and a garbage count - so this case is about surviving, not about
     * what allocate returns for it. */
    PMIX_INFO_LOAD(&netdir, PMIX_ALLOC_NETWORK, "not-an-array", PMIX_STRING);
    rc = make_blob(TESTNS "-bad1", &netdir, 1, &blob, &nblob);
    ok(PMIX_SUCCESS == rc || PMIX_ERR_NOT_FOUND == rc,
       "a scalar where the network array belongs is survived");
    if (PMIX_SUCCESS == rc) {
        PMIX_INFO_FREE(blob, nblob);
        blob = NULL;
    }
    PMIX_INFO_DESTRUCT(&netdir);

    /* An array of the right kind but the wrong element type is the harder
     * one: it passes any "is it a data array?" test, and a pmix_info_t is
     * wider than a char*, so the walk runs off the end of the allocation. */
    PMIX_DATA_ARRAY_CREATE(darray, 2, PMIX_STRING);
    ((char **) darray->array)[0] = strdup("pmix.alloc.nsec");
    ((char **) darray->array)[1] = strdup("true");
    PMIX_LOAD_KEY(netdir.key, PMIX_ALLOC_NETWORK);
    netdir.flags = 0;
    netdir.value.type = PMIX_DATA_ARRAY;
    netdir.value.data.darray = darray;
    rc = make_blob(TESTNS "-bad2", &netdir, 1, &blob, &nblob);
    ok(PMIX_SUCCESS == rc || PMIX_ERR_NOT_FOUND == rc,
       "an array of the wrong element type is survived");
    if (PMIX_SUCCESS == rc) {
        PMIX_INFO_FREE(blob, nblob);
        blob = NULL;
    }
    PMIX_INFO_DESTRUCT(&netdir);

    /* --- naming the HFI to the fabric software --- */

    rc = register_nspace();
    ok(PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc, "the namespace registers");
    if (PMIX_SUCCESS == rc || PMIX_OPERATION_SUCCEEDED == rc) {
        PMIX_LOAD_PROCID(&proc, TESTNS, 0);
        env = NULL;
        rc = pmix_pnet_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank with an HFI");
        ev = envval(env, "PSM3_NIC");
        ok(NULL != ev && 0 == strcmp(ev, "hfi1_0"),
           "PSM3 is told the HFI by name");
        PMIx_Argv_free(env);
        env = NULL;

        /* Intel also makes ethernet controllers, and they report class
         * 0200 rather than 0208.  Claiming one would put the process on
         * hardware nobody gave it. */
        PMIX_LOAD_PROCID(&proc, TESTNS, 1);
        env = NULL;
        rc = pmix_pnet_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank with no HFI");
        ok(NULL == envval(env, "PSM3_NIC"),
           "and says nothing about an ethernet controller from the same vendor");
        PMIx_Argv_free(env);
        env = NULL;
    }

    PMIx_server_finalize();
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
