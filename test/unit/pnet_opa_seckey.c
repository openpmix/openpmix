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
 * The same request carries OPX's job key, FI_OPX_UUID.  That one belongs
 * to a job and the jobs it spawns - they have to share it to reach each
 * other through shared memory, and nothing else should - and it is never
 * written over a value the child's environment already has.
 *
 * It runs against opa-hfi.xml, handed to the server through the documented
 * topo_file hook.  That fixture carries Omni-Path HFIs (PCI 8086
 * class 0208: Cornelis makes them, under Intel's vendor id), which is what
 * puts opa into the active list on a machine with no such card, and an
 * Intel *ethernet* controller (class 0200) beside them, because "same
 * vendor, wrong class" is the mistake the component's PCI filter exists
 * to prevent.  Its HFIs are hfi1_0, hfi1_1 and hfi1_10: OPX finds unit 1's
 * device by comparing "hfi1_1" over its own length, so on that node unit 1
 * may not be named to it.
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

#define NPROCS 5
#define TESTNS "pnet-opa"
#define XPORT  "OMPI_MCA_orte_precondition_transports"
#define OPXKEY "FI_OPX_UUID"
#define OPXSEL "FI_OPX_HFI_SELECT"

/* rank 0 gets an HFI, rank 1 the ethernet controller from the same
 * vendor - which opa must not claim - rank 2 two HFIs, rank 3 the HFI
 * whose name another one begins with, and rank 4 that other one */
static const char *osnames[NPROCS][2] = {{"hfi1_0", NULL}, {"enp66s0", NULL},
                                         {"hfi1_0", "hfi1_1"}, {"hfi1_1", NULL},
                                         {"hfi1_10", NULL}};

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

/* OPX reads its key as at most 32 hex digits, and falls back to its
 * built-in default on anything else */
static bool is_opx_key(const char *val)
{
    size_t n;

    if (NULL == val || 32 != strlen(val)) {
        return false;
    }
    for (n = 0; '\0' != val[n]; n++) {
        if (!isxdigit((unsigned char) val[n])) {
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

/* The value of "var" in the environment rank 0 of nspace would be forked
 * with, starting from "preset" (which may be NULL).  The caller frees the
 * returned string. */
static char *child_value(const char *nspace, const char *var, const char *preset)
{
    char **env = NULL;
    const char *ev;
    pmix_proc_t proc;
    pmix_status_t rc;
    char *answer = NULL;

    if (NULL != preset) {
        PMIx_Argv_append_nosize(&env, preset);
    }
    PMIX_LOAD_PROCID(&proc, nspace, 0);
    rc = pmix_pnet_base_setup_fork(&proc, &env);
    if (PMIX_SUCCESS == rc) {
        ev = envval(env, var);
        if (NULL != ev) {
            answer = strdup(ev);
        }
    }
    PMIx_Argv_free(env);
    return answer;
}

/* Ask allocate for a seckey and deliver the blob */
static bool deliver(char *nspace, pmix_info_t *directives, size_t ndirs)
{
    pmix_info_t *blob = NULL;
    size_t nblob = 0;
    pmix_status_t rc;

    if (PMIX_SUCCESS != make_blob(nspace, directives, ndirs, &blob, &nblob)) {
        return false;
    }
    rc = pmix_pnet.setup_local_network(nspace, blob, nblob);
    PMIX_INFO_FREE(blob, nblob);
    return (PMIX_SUCCESS == rc);
}

/* ...and report the transport key the child would be forked with */
static char *seckey_round_trip(char *nspace, pmix_info_t *directives, size_t ndirs)
{
    if (!deliver(nspace, directives, ndirs)) {
        return NULL;
    }
    return child_value(nspace, XPORT, NULL);
}

/* ...or the OPX key, for a job spawned by the given process (by nobody if
 * pnspace is NULL) */
static char *opx_key_for(char *nspace, const char *pnspace, pmix_rank_t prank)
{
    pmix_info_t dirs[2];
    pmix_proc_t parent;
    bool flag = true;
    size_t ndirs = 1;
    char *answer = NULL;

    PMIX_INFO_LOAD(&dirs[0], PMIX_SETUP_APP_ALL, &flag, PMIX_BOOL);
    if (NULL != pnspace) {
        PMIX_LOAD_PROCID(&parent, pnspace, prank);
        PMIX_INFO_LOAD(&dirs[1], PMIX_PARENT_ID, &parent, PMIX_PROC);
        ++ndirs;
    }
    if (deliver(nspace, dirs, ndirs)) {
        answer = child_value(nspace, OPXKEY, NULL);
    }
    PMIX_INFO_DESTRUCT(&dirs[0]);
    if (NULL != pnspace) {
        PMIX_INFO_DESTRUCT(&dirs[1]);
    }
    return answer;
}

/* Register a namespace whose ranks were mapped against devices, the way
 * pnet_assigned_devices.c does - rank 0 an HFI, rank 1 the ethernet
 * controller from the same vendor, rank 2 both HFIs. */
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
        size_t k = 0, ndev, d;

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

        ndev = (NULL == osnames[m][1]) ? 1 : 2;
        PMIX_DATA_ARRAY_CREATE(darray, ndev, PMIX_DEVICE);
        dev = (pmix_device_t *) darray->array;
        for (d = 0; d < ndev; d++) {
            PMIx_Device_construct(&dev[d]);
            dev[d].osname = strdup(osnames[m][d]);
            pmix_asprintf(&dev[d].uuid, "nic://%s::%s", pmix_globals.hostname, osnames[m][d]);
            dev[d].type = (1 == m) ? PMIX_DEVTYPE_NETWORK : PMIX_DEVTYPE_OPENFABRICS;
        }
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

/* Is anything stored as job data under PMIX_CREDENTIAL for this job?
 * The datastore belongs to the progress thread, so ask from there. */
typedef struct {
    pmix_event_t ev;
    pmix_lock_t lock;
    const char *nspace;
    bool found;
} credq_t;

static void do_credq(int sd, short args, void *cbdata)
{
    credq_t *q = (credq_t *) cbdata;
    pmix_proc_t wildcard;
    pmix_cb_t cb;
    pmix_status_t rc;

    (void) sd;
    (void) args;
    PMIX_LOAD_PROCID(&wildcard, q->nspace, PMIX_RANK_WILDCARD);
    PMIX_CONSTRUCT(&cb, pmix_cb_t);
    cb.proc = &wildcard;
    cb.key = PMIX_CREDENTIAL;
    cb.scope = PMIX_INTERNAL;
    PMIX_GDS_FETCH_KV(rc, pmix_globals.mypeer, &cb);
    q->found = (PMIX_SUCCESS == rc && 0 < pmix_list_get_size(&cb.kvs));
    cb.key = NULL;
    cb.proc = NULL;
    PMIX_DESTRUCT(&cb);
    PMIX_WAKEUP_THREAD(&q->lock);
}

static bool job_holds_credential(const char *nspace)
{
    credq_t q;

    memset(&q, 0, sizeof(q));
    PMIX_CONSTRUCT_LOCK(&q.lock);
    q.nspace = nspace;
    PMIX_THREADSHIFT(&q, do_credq);
    PMIX_WAIT_THREAD(&q.lock);
    PMIX_DESTRUCT_LOCK(&q.lock);
    return q.found;
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
    char *key1 = NULL, *key2 = NULL, *keya = NULL, *keyb = NULL;
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
    ok(has_key(blob, nblob, PMIX_PNET_OPA_BLOB),
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
    /* the key reaches the child through its environment only - it is
     * not job data another process could read */
    ok(!job_holds_credential(TESTNS), "the key is not stored as job data");

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

    /* --- OPX's key: a job's and the jobs it spawns --- */

    key1 = child_value(TESTNS "-net", OPXKEY, NULL);
    key2 = child_value(TESTNS "-two", OPXKEY, NULL);
    ok(is_opx_key(key1), "the child is forked with a key OPX will accept");
    ok(NULL != key1 && NULL != key2 && 0 != strcmp(key1, key2),
       "two jobs launched apart do not share one");
    free(key2);

    keya = opx_key_for(TESTNS "-par", NULL, 0);
    ok(is_opx_key(keya), "a job with no parent gets a well-formed key");
    key2 = opx_key_for(TESTNS "-par", NULL, 0);
    ok(NULL != keya && NULL != key2 && 0 == strcmp(keya, key2),
       "and the same one when it is set up again");
    free(key2);
    keyb = opx_key_for(TESTNS "-kid", TESTNS "-par", 0);
    ok(NULL != keya && NULL != keyb && 0 == strcmp(keya, keyb),
       "a job it spawns gets its key");
    free(keyb);
    keyb = opx_key_for(TESTNS "-grandkid", TESTNS "-kid", 3);
    ok(NULL != keya && NULL != keyb && 0 == strcmp(keya, keyb),
       "and so does a job that one spawns");
    free(keyb);
    key2 = opx_key_for(TESTNS "-other", NULL, 0);
    ok(is_opx_key(key2) && NULL != keya && 0 != strcmp(key2, keya),
       "a job launched apart gets another key");
    free(key2);
    /* a parent this server set up no job for gives nothing to inherit */
    key2 = opx_key_for(TESTNS "-orphan", "pnet-opa-nosuch", 0);
    ok(is_opx_key(key2) && NULL != keya && 0 != strcmp(key2, keya),
       "a job naming a parent it knows nothing of gets a key of its own");
    free(key2);
    free(keya);

    /* a key the child's environment already carries was put there on
     * purpose - by the user, for the app - and is not replaced */
    key2 = child_value(TESTNS "-net", OPXKEY, OPXKEY "=0123abcd");
    ok(NULL != key2 && 0 == strcmp(key2, "0123abcd"),
       "a key already in the child's environment is left alone");
    free(key2);

    /* and one the user set where the job was launched travels in the blob
     * after ours, so it is the one the child gets */
    setenv(OPXKEY, "feedface", 1);
    key2 = opx_key_for(TESTNS "-user", NULL, 0);
    unsetenv(OPXKEY);
    ok(NULL != key2 && 0 == strcmp(key2, "feedface"),
       "a key set where the job was launched is the one used");
    free(key2);

    /* nothing is kept for a job that is gone */
    pmix_pnet.deregister_nspace(TESTNS "-two");
    key2 = child_value(TESTNS "-two", OPXKEY, NULL);
    ok(NULL == key2, "a deregistered job's key is forgotten");
    free(key2);
    free(key1);

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
        ev = envval(env, OPXSEL);
        ok(NULL != ev && 0 == strcmp(ev, "0"),
           "OPX is told the HFI by the unit number its name carries");
        /* PSM3 is Intel's library for its ethernet adapters, not an
         * Omni-Path one */
        ok(NULL == envval(env, "PSM3_NIC"), "and PSM3 is not told anything");
        PMIx_Argv_free(env);
        env = NULL;

        /* Intel also makes ethernet controllers, and they report class
         * 0200 rather than 0208.  Claiming one would put the process on
         * hardware nobody gave it. */
        PMIX_LOAD_PROCID(&proc, TESTNS, 1);
        env = NULL;
        rc = pmix_pnet_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank with no HFI");
        ok(NULL == envval(env, OPXSEL),
           "and says nothing about an ethernet controller from the same vendor");
        ok(NULL != envval(env, OPXKEY),
           "but still hands it the job's key, which OPX needs regardless");
        PMIx_Argv_free(env);
        env = NULL;

        /* OPX opens the first fixed unit in a list and stops, so a list
         * would say "the first of these" rather than "these".  A process
         * given two is left unset. */
        PMIX_LOAD_PROCID(&proc, TESTNS, 2);
        env = NULL;
        rc = pmix_pnet_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank with two HFIs");
        ok(NULL == envval(env, OPXSEL), "and does not write a list of units OPX would read as one");
        PMIx_Argv_free(env);
        env = NULL;

        /* OPX would look for unit 1 as anything beginning "hfi1_1", and
         * this node has an hfi1_10 */
        PMIX_LOAD_PROCID(&proc, TESTNS, 3);
        env = NULL;
        rc = pmix_pnet_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank on hfi1_1");
        ok(NULL == envval(env, OPXSEL),
           "and does not name a unit OPX could take for another device");
        PMIx_Argv_free(env);
        env = NULL;

        PMIX_LOAD_PROCID(&proc, TESTNS, 4);
        env = NULL;
        rc = pmix_pnet_base_setup_fork(&proc, &env);
        ok(PMIX_SUCCESS == rc, "setup_fork succeeds for a rank on hfi1_10");
        ev = envval(env, OPXSEL);
        ok(NULL != ev && 0 == strcmp(ev, "10"),
           "and names unit 10, which no other device's name begins with");
        PMIx_Argv_free(env);
        env = NULL;

        /* a device assignment is the more specific request, so it is
         * written over a selection the environment already carries */
        PMIX_LOAD_PROCID(&proc, TESTNS, 0);
        env = NULL;
        PMIx_Argv_append_nosize(&env, OPXSEL "=1");
        rc = pmix_pnet_base_setup_fork(&proc, &env);
        ev = envval(env, OPXSEL);
        ok(PMIX_SUCCESS == rc && NULL != ev && 0 == strcmp(ev, "0"),
           "the assigned unit replaces one already in the environment");
        PMIx_Argv_free(env);
        env = NULL;
    }

    PMIx_server_finalize();
    fprintf(stderr, "%s: %d checks, %d failures\n",
            (0 == failures) ? "PASS" : "FAIL", checks, failures);
    return (0 == failures) ? 0 : 1;
}
